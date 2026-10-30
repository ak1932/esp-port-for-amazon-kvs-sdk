/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "common_defs.h"
#include "file_credential_provider.h"

typedef struct {
    UINT32 calls;
    UINT64 expiration;
    STATUS result;
    CHAR accessKey[24];
} FakeFetch;

static STATUS fakeFetch(UINT64 customData, PCHAR *pAccessKey, PUINT32 pAccessKeyLen, PCHAR *pSecretKey, PUINT32 pSecretKeyLen,
                        PCHAR *pSessionToken, PUINT32 pSessionTokenLen, PUINT64 pExpiration)
{
    FakeFetch *pFake = (FakeFetch *) HANDLE_TO_POINTER(customData);

    pFake->calls++;
    if (STATUS_FAILED(pFake->result)) {
        return pFake->result;
    }
    /* A different key per fetch, so a stale pointer can be told from a fresh one. */
    SNPRINTF(pFake->accessKey, SIZEOF(pFake->accessKey), "AKIAFETCH%" PRIu32, pFake->calls);
    *pAccessKey = pFake->accessKey;
    *pAccessKeyLen = (UINT32) STRLEN(pFake->accessKey);
    *pSecretKey = (PCHAR) "secret";
    *pSecretKeyLen = 6;
    *pSessionToken = (PCHAR) "token";
    *pSessionTokenLen = 5;
    *pExpiration = pFake->expiration;
    return STATUS_SUCCESS;
}

TEST_CASE("Callback provider: fetches once while the credentials are fresh", "[credential]")
{
    FakeFetch fake = {.expiration = GETTIME() + 3600ULL * HUNDREDS_OF_NANOS_IN_A_SECOND};
    PAwsCredentialProvider pProvider = NULL;
    PAwsCredentials pFirst = NULL, pAgain = NULL;

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, createCallbackCredentialProvider(fakeFetch, POINTER_TO_HANDLE(&fake), &pProvider));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pProvider->getCredentialsFn(pProvider, &pFirst));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pProvider->getCredentialsFn(pProvider, &pAgain));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pProvider->getCredentialsFn(pProvider, &pAgain));

    TEST_ASSERT_EQUAL_UINT32(1, fake.calls);
    TEST_ASSERT_EQUAL_PTR(pFirst, pAgain);
    TEST_ASSERT_EQUAL_STRING_LEN("AKIAFETCH1", pFirst->accessKeyId, pFirst->accessKeyIdLen);
    TEST_ASSERT_EQUAL_UINT64(fake.expiration, pFirst->expiration);

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, freeCallbackCredentialProvider(&pProvider));
    TEST_ASSERT_NULL(pProvider);
}

TEST_CASE("Callback provider: near expiry it fetches again and keeps the previous credentials", "[credential]")
{
    /* Inside the grace period from the start, so every get fetches. */
    FakeFetch fake = {.expiration = GETTIME() + HUNDREDS_OF_NANOS_IN_A_SECOND};
    PAwsCredentialProvider pProvider = NULL;
    PAwsCredentials pFirst = NULL, pSecond = NULL;

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, createCallbackCredentialProvider(fakeFetch, POINTER_TO_HANDLE(&fake), &pProvider));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pProvider->getCredentialsFn(pProvider, &pFirst));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pProvider->getCredentialsFn(pProvider, &pSecond));

    TEST_ASSERT_EQUAL_UINT32(2, fake.calls);
    TEST_ASSERT_NOT_EQUAL(pFirst, pSecond);
    TEST_ASSERT_EQUAL_STRING_LEN("AKIAFETCH2", pSecond->accessKeyId, pSecond->accessKeyIdLen);
    /* A user that took the first credentials can still sign with them. */
    TEST_ASSERT_EQUAL_STRING_LEN("AKIAFETCH1", pFirst->accessKeyId, pFirst->accessKeyIdLen);

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, freeCallbackCredentialProvider(&pProvider));
}

TEST_CASE("Callback provider: a failed fetch hands out nothing", "[credential]")
{
    FakeFetch fake = {.result = STATUS_INVALID_ARG};
    PAwsCredentialProvider pProvider = NULL;
    PAwsCredentials pCredentials = (PAwsCredentials) &fake;

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, createCallbackCredentialProvider(fakeFetch, POINTER_TO_HANDLE(&fake), &pProvider));
    TEST_ASSERT_EQUAL_HEX32(STATUS_INVALID_ARG, pProvider->getCredentialsFn(pProvider, &pCredentials));
    TEST_ASSERT_NULL(pCredentials);

    /* It recovers on the next get. */
    fake.result = STATUS_SUCCESS;
    fake.expiration = GETTIME() + 3600ULL * HUNDREDS_OF_NANOS_IN_A_SECOND;
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pProvider->getCredentialsFn(pProvider, &pCredentials));
    TEST_ASSERT_NOT_NULL(pCredentials);
    TEST_ASSERT_EQUAL_UINT32(2, fake.calls);

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, freeCallbackCredentialProvider(&pProvider));
}
