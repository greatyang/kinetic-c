#include "KineticApi.h"
#include "unity.h"
#include <stdio.h>
#include <protobuf-c/protobuf-c.h>
#include "KineticProto.h"
#include "mock_KineticMessage.h"
#include "mock_KineticLogger.h"
#include "mock_KineticConnection.h"
#include "mock_KineticExchange.h"

#define TEST_ASSERT_EQUAL_KINETIC_STATUS(expected, actual) \
if (expected != actual) { \
    char err[128]; \
    sprintf(err, "Expected Kinetic status code of %s(%d), Was %s(%d)", \
        protobuf_c_enum_descriptor_get_value( \
            &KineticProto_status_status_code_descriptor, expected)->name, \
        expected, \
        protobuf_c_enum_descriptor_get_value( \
            &KineticProto_status_status_code_descriptor, actual)->name, \
        actual); \
    TEST_FAIL_MESSAGE(err); \
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_KineticApi_Init_should_initialize_the_logger(void)
{
    KineticLogger_Init_Expect("some/file.log");

    KineticApi_Init("some/file.log");
}

void test_KineticApi_Connect_should_create_a_connection(void)
{
    KineticConnection dummy;
    KineticConnection result;

    dummy.Connected = false; // Ensure gets set appropriately by internal connect call

    KineticConnection_Create_ExpectAndReturn(dummy);
    KineticConnection_Connect_ExpectAndReturn(&dummy, "somehost.com", 321, true, true);

    result = KineticApi_Connect("somehost.com", 321, true);

    TEST_ASSERT_TRUE(result.Connected);
}

void test_KineticApi_Connect_should_log_a_failed_connection(void)
{
    KineticConnection dummy;
    KineticConnection result;

    // Ensure appropriately updated per internal connect call result
    dummy.Connected = true;
    dummy.FileDescriptor = 333;

    KineticConnection_Create_ExpectAndReturn(dummy);
    KineticConnection_Connect_ExpectAndReturn(&dummy, "somehost.com", 123, true, false);
    KineticLogger_Log_Expect("Failed creating connection to somehost.com:123");

    result = KineticApi_Connect("somehost.com", 123, true);

    TEST_ASSERT_FALSE(result.Connected);
    TEST_ASSERT_EQUAL(-1, result.FileDescriptor);
}

void test_KineticApi_SendNoop_should_send_NOOP_command(void)
{
    KineticConnection connection;
    KineticExchange exchange;
    KineticMessage message;
    KineticProto_Status_StatusCode status;
    int64_t identity = 1234;
    int64_t connectionID = 5678;

    KineticConnection_Create_ExpectAndReturn(connection);
    KineticConnection_Connect_ExpectAndReturn(&connection, "salgood.com", 88, false, true);

    connection = KineticApi_Connect("salgood.com", 88, false);

    KineticExchange_Init_Expect(&exchange, identity, connectionID);
    KineticMessage_Init_Expect(&message, &exchange);
    KineticConnection_SendMessage_ExpectAndReturn(&connection, &message, true);

    status = KineticApi_SendNoop(&connection, &exchange, &message);

    TEST_IGNORE_MESSAGE("Finish imlementation!");

    TEST_ASSERT_EQUAL_KINETIC_STATUS(KINETIC_PROTO_STATUS_STATUS_CODE_SUCCESS, status);
    TEST_ASSERT_EQUAL(KINETIC_PROTO_STATUS_STATUS_CODE_SUCCESS, status);
}