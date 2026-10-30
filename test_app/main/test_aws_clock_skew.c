/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "common_defs.h"
#include "aws_clock_skew.h"

/* "Fri, 27 Jun 2025 12:27:54 GMT" is epoch 1751027274 */
#define DATE_HDR    ((PCHAR) "Fri, 27 Jun 2025 12:27:54 GMT")
#define DATE_EPOCH  1751027274ULL
#define SEC(s)      ((UINT64) (s) * HUNDREDS_OF_NANOS_IN_A_SECOND)

TEST_CASE("Clock skew: within the threshold is ignored", "[aws_clock_skew]")
{
    UINT64 skew = 1, serverTime = 0;
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, awsClockSkewFromDate(DATE_HDR, SEC(DATE_EPOCH + 120), &skew, &serverTime));
    TEST_ASSERT_EQUAL_UINT64(0, skew);
    TEST_ASSERT_EQUAL_UINT64(SEC(DATE_EPOCH), serverTime);
}

TEST_CASE("Clock skew: device behind the server", "[aws_clock_skew]")
{
    UINT64 skew = 0;
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, awsClockSkewFromDate(DATE_HDR, SEC(DATE_EPOCH - 600), &skew, NULL));
    TEST_ASSERT_EQUAL_UINT64(SEC(600), skew);
}

TEST_CASE("Clock skew: device ahead of the server", "[aws_clock_skew]")
{
    UINT64 skew = 0;
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, awsClockSkewFromDate(DATE_HDR, SEC(DATE_EPOCH + 3600), &skew, NULL));
    TEST_ASSERT_EQUAL_UINT64(SEC(3600) | AWS_CLOCK_SKEW_DEVICE_AHEAD, skew);
}

TEST_CASE("Clock skew: applied to a request time", "[aws_clock_skew]")
{
    RequestInfo info = {0};

    info.currentTime = SEC(DATE_EPOCH - 600);
    awsApplyClockSkew(&info, SEC(600));
    TEST_ASSERT_EQUAL_UINT64(SEC(DATE_EPOCH), info.currentTime);

    info.currentTime = SEC(DATE_EPOCH + 3600);
    awsApplyClockSkew(&info, SEC(3600) | AWS_CLOCK_SKEW_DEVICE_AHEAD);
    TEST_ASSERT_EQUAL_UINT64(SEC(DATE_EPOCH), info.currentTime);

    awsApplyClockSkew(&info, 0);
    TEST_ASSERT_EQUAL_UINT64(SEC(DATE_EPOCH), info.currentTime);
}

TEST_CASE("Clock skew: unparsable date", "[aws_clock_skew]")
{
    UINT64 skew = 1;
    TEST_ASSERT_EQUAL(STATUS_INVALID_ARG, awsClockSkewFromDate((PCHAR) "not a date", SEC(DATE_EPOCH), &skew, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, skew);
    TEST_ASSERT_EQUAL(STATUS_NULL_ARG, awsClockSkewFromDate(NULL, 0, &skew, NULL));
}
