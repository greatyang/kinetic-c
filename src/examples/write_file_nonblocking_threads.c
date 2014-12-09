/*
* kinetic-c
* Copyright (C) 2014 Seagate Technology.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*/
#include "kinetic_client.h"
#include "kinetic_types.h"
#include "byte_array.h"
#include <stdlib.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <pthread.h>
#include <errno.h>

#define REPORT_ERRNO(en, msg) if(en != 0){errno = en; perror(msg);}

struct kinetic_thread_arg {
    char ip[16];
    struct kinetic_put_arg* opArgs;
    int opCount;
};

typedef struct {
    size_t opsInProgress;
    size_t currentChunk;
    size_t maxOverlappedChunks;
    int fd;
    ByteBuffer keyPrefix;
    uint8_t keyPrefixBuffer[KINETIC_DEFAULT_KEY_LEN];
    pthread_mutex_t transferMutex;
    pthread_mutex_t completeMutex;
    pthread_cond_t completeCond;
    KineticStatus status;
    KineticSession* session;
} FileTransferProgress;

typedef struct {
    KineticEntry entry;
    uint8_t key[KINETIC_DEFAULT_KEY_LEN];
    uint8_t value[KINETIC_OBJ_SIZE];
    uint8_t tag[KINETIC_DEFAULT_KEY_LEN];
    FileTransferProgress* currentTransfer;
} AsyncWriteClosureData;

typedef struct {
    KineticSession* session;
    const char* filename;
    const size_t maxOverlappedChunks;
    uint64_t keyPrefix;
    pthread_t thread;
    KineticStatus status;
} StoreFileOperation;

void* store_file_thread(void* storeArgs);
FileTransferProgress* start_file_transfer(KineticSession* session,
    char const * const filename, uint64_t prefix, uint32_t maxOverlappedChunks);
KineticStatus wait_for_transfer_complete(FileTransferProgress* const transfer);

static int put_chunk_of_file(FileTransferProgress* transfer);
static void put_chunk_of_file_finished(KineticCompletionData* kinetic_data, void* client_data);

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    // Initialize kinetic-c and configure session
    const char HmacKeyString[] = "asdfasdf";
    KineticSession session = {
        .config = (KineticSessionConfig) {
            .host = "localhost",
            .port = KINETIC_PORT,
            .clusterVersion = 0,
            .identity = 1,
            .hmacKey = ByteArray_CreateWithCString(HmacKeyString),
        }
    };
    KineticClient_Init("stdout", 0);

    // Establish connection
    KineticStatus status = KineticClient_CreateConnection(&session);
    if (status != KINETIC_STATUS_SUCCESS) {
        fprintf(stdout, "Failed connecting to the Kinetic device w/status: %s\n",
            Kinetic_GetStatusDescription(status));
        return -1;
    }

    // Create a unique/common key prefix
    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t prefix = (uint64_t)now.tv_sec << sizeof(8);

    // Store the file(s) and wait for completion
    bool success = true;
    const int maxOverlappedChunks = 15;

    StoreFileOperation ops[] = {
        {
            .session = &session,
            .filename = "./test/support/data/file_a.png",
            .keyPrefix = prefix,
            .maxOverlappedChunks = maxOverlappedChunks,
        },
        {
            .session = &session,
            .filename = "./test/support/data/file_b.png",
            .keyPrefix = prefix,
            .maxOverlappedChunks = maxOverlappedChunks,
        },
        {
            .session = &session,
            .filename = "./test/support/data/file_c.png",
            .keyPrefix = prefix,
            .maxOverlappedChunks = maxOverlappedChunks,
        },
    };
    const int numFiles = sizeof(ops) / sizeof(StoreFileOperation);
    for (int i = 0; i < numFiles; i++) {
        printf("Storing '%s' to disk...\n", ops[i].filename);
        int pthreadStatus = pthread_create(&ops[i].thread, NULL, store_file_thread, &ops[i]);
        if (pthreadStatus != 0) {
            REPORT_ERRNO(pthreadStatus, "pthread_create");
            fprintf(stdout, "Failed creating store thread for '%s'!\n", ops[i].filename);
            success = false;
        }
    }
    for (int i = 0; i < numFiles; i++) {
        int pthreadStatus = pthread_join(ops[i].thread, NULL);
        if (pthreadStatus == 0) {
            printf("File '%s' stored successfully!\n", ops[i].filename);
        }
        else {
            REPORT_ERRNO(pthreadStatus, "pthread_join");
            fprintf(stdout, "Failed storing '%s' to disk! status: %s\n",
                ops[i].filename, Kinetic_GetStatusDescription(ops[i].status));
            success = false;
        }
    }
    printf("Complete!\n");
    
    // Shutdown client connection and cleanup
    KineticClient_DestroyConnection(&session);
    KineticClient_Shutdown();

    return success ? 0 : -1;
}

void* store_file_thread(void* storeArgs)
{
    // Kick off the chained write/PUT operations and wait for completion
    StoreFileOperation* op = storeArgs;
    FileTransferProgress* transfer =
        start_file_transfer(op->session, op->filename, op->keyPrefix, op->maxOverlappedChunks);
    op->status = wait_for_transfer_complete(transfer);
    if (op->status != KINETIC_STATUS_SUCCESS) {
        fprintf(stdout, "Transfer failed w/status: %s\n", Kinetic_GetStatusDescription(op->status));
    }

    return (void*)storeArgs;
}

FileTransferProgress * start_file_transfer(KineticSession* session,
    char const * const filename, uint64_t prefix, uint32_t maxOverlappedChunks)
{
    FileTransferProgress * transferState = malloc(sizeof(FileTransferProgress));
    *transferState = (FileTransferProgress) {
        .session = session,
        .maxOverlappedChunks = maxOverlappedChunks,
        .keyPrefix = ByteBuffer_CreateAndAppend(transferState->keyPrefixBuffer,
            sizeof(transferState->keyPrefixBuffer), &prefix, sizeof(prefix)),
        .fd = open(filename, O_RDONLY),
    };

    pthread_mutex_init(&transferState->transferMutex, NULL);
    pthread_mutex_init(&transferState->completeMutex, NULL); 
    pthread_cond_init(&transferState->completeCond, NULL);
        
    // Start max overlapped PUT operations
    for (size_t i = 0; i < transferState->maxOverlappedChunks; i++) {
        put_chunk_of_file(transferState);
    }
    return transferState;
}

KineticStatus wait_for_transfer_complete(FileTransferProgress* const transfer)
{
    pthread_mutex_lock(&transfer->completeMutex);
    pthread_cond_wait(&transfer->completeCond, &transfer->completeMutex);
    pthread_mutex_unlock(&transfer->completeMutex);

    KineticStatus status = transfer->status;

    pthread_mutex_destroy(&transfer->completeMutex);
    pthread_cond_destroy(&transfer->completeCond);

    close(transfer->fd);
    
    pthread_mutex_destroy(&transfer->transferMutex);
    free(transfer);

    return status;
}

int put_chunk_of_file(FileTransferProgress* transfer)
{
    AsyncWriteClosureData* closureData = calloc(1, sizeof(AsyncWriteClosureData));

    pthread_mutex_lock(&transfer->transferMutex);

    transfer->opsInProgress++;
    closureData->currentTransfer = transfer;

    int bytesRead = read(transfer->fd, closureData->value, sizeof(closureData->value));
    if (bytesRead > 0) {
        transfer->currentChunk++;
        closureData->entry = (KineticEntry){
            .key = ByteBuffer_CreateAndAppend(closureData->key, sizeof(closureData->key),
                transfer->keyPrefix.array.data, transfer->keyPrefix.bytesUsed),
            .tag = ByteBuffer_CreateAndAppendFormattedCString(closureData->tag, sizeof(closureData->tag),
                "some_value_tag..._%04d", transfer->currentChunk),
            .algorithm = KINETIC_ALGORITHM_SHA1,
            .value = ByteBuffer_Create(closureData->value, sizeof(closureData->value), (size_t)bytesRead),
            .synchronization = KINETIC_SYNCHRONIZATION_WRITETHROUGH,
        };
        KineticStatus status = KineticClient_Put(transfer->session, &closureData->entry,
            &(KineticCompletionClosure) {
                .callback = put_chunk_of_file_finished,
                .clientData = closureData,
            });
        if (status != KINETIC_STATUS_SUCCESS) {
            transfer->opsInProgress--;
            free(closureData);
            fprintf(stdout, "Failed writing chunk! PUT request reported status: %s\n",
                Kinetic_GetStatusDescription(status));
        }
    }
    else if (bytesRead == 0) { // EOF reached
        transfer->opsInProgress--;
        free(closureData);
    }
    else {
        transfer->opsInProgress--;
        free(closureData);
        fprintf(stdout, "Failed reading data from file!\n");
        REPORT_ERRNO(bytesRead, "read");
    }

    pthread_mutex_unlock(&transfer->transferMutex);
    
    return bytesRead;
}

void put_chunk_of_file_finished(KineticCompletionData* kinetic_data, void* clientData)
{
    AsyncWriteClosureData* closureData = clientData;
    FileTransferProgress* transfer = closureData->currentTransfer;
    free(closureData);

    pthread_mutex_lock(&transfer->transferMutex);
    transfer->opsInProgress--;
    pthread_mutex_unlock(&transfer->transferMutex);

    if (kinetic_data->status == KINETIC_STATUS_SUCCESS) {
        int bytesPut = put_chunk_of_file(transfer);
        if (bytesPut <= 0 && transfer->opsInProgress == 0) {
            if (transfer->status == KINETIC_STATUS_NOT_ATTEMPTED) {
                transfer->status = KINETIC_STATUS_SUCCESS;
            }
            pthread_cond_signal(&transfer->completeCond);
        }
    }
    else {
        transfer->status = kinetic_data->status;
        // only signal when finished
        // keep track of outstanding operations
        // if there is no more data to read (or error), and no outstanding operations,
        // then signal
        pthread_cond_signal(&transfer->completeCond);
        fprintf(stdout, "Failed writing chunk! PUT response reported status: %s\n",
            Kinetic_GetStatusDescription(kinetic_data->status));
    }
}
