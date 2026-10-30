/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "common_defs.h"
#include "util.h"

static void expect_url(const char *region, const char *expected)
{
    CHAR url[256];
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, getKvsControlPlaneUrl((PCHAR) region, url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING(expected, url);
}

TEST_CASE("Control-plane URL: commercial region", "[kvs_endpoint]")
{
    expect_url("us-east-1", "https://kinesisvideo.us-east-1.amazonaws.com");
    expect_url("ap-south-1", "https://kinesisvideo.ap-south-1.amazonaws.com");
}

TEST_CASE("Control-plane URL: China region", "[kvs_endpoint]")
{
    expect_url("cn-north-1", "https://kinesisvideo.cn-north-1.amazonaws.com.cn");
}

TEST_CASE("Control-plane URL: GovCloud, ISO and ISO-B use FIPS", "[kvs_endpoint]")
{
    expect_url("us-gov-west-1", "https://kinesisvideo-fips.us-gov-west-1.amazonaws.com");
    expect_url("us-iso-east-1", "https://kinesisvideo-fips.us-iso-east-1.c2s.ic.gov");
    expect_url("us-isob-east-1", "https://kinesisvideo-fips.us-isob-east-1.sc2s.sgov.gov");
}

TEST_CASE("Control-plane URL: bad arguments", "[kvs_endpoint]")
{
    CHAR url[16];
    TEST_ASSERT_EQUAL(STATUS_NULL_ARG, getKvsControlPlaneUrl(NULL, url, sizeof(url)));
    TEST_ASSERT_EQUAL(STATUS_INVALID_ARG, getKvsControlPlaneUrl((PCHAR) "", url, sizeof(url)));
    TEST_ASSERT_EQUAL(STATUS_BUFFER_TOO_SMALL, getKvsControlPlaneUrl((PCHAR) "us-east-1", url, sizeof(url)));
}
