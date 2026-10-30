/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "common_defs.h"
#include "auth.h"
#include "credential_provider_view.h"
#include "file_credential_provider.h"

/* A shared provider whose refreshes the test drives, as another user's gets would. */
typedef struct {
    AwsCredentialProvider credentialProvider;
    PAwsCredentials pAwsCredentials;
    PAwsCredentials pPrevAwsCredentials;
    UINT32 gets;
    UINT32 refreshes;
} FakeShared;

static STATUS fakeSharedGet(PAwsCredentialProvider pCredentialProvider, PAwsCredentials *ppAwsCredentials)
{
    FakeShared *pFake = (FakeShared *) pCredentialProvider;

    pFake->gets++;
    *ppAwsCredentials = pFake->pAwsCredentials;
    return STATUS_SUCCESS;
}

static void fakeSharedRefresh(FakeShared *pFake)
{
    CHAR accessKey[24];
    PAwsCredentials pNew = NULL;

    pFake->refreshes++;
    SNPRINTF(accessKey, SIZEOF(accessKey), "AKIASHARED%" PRIu32, pFake->refreshes);
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS,
                            aws_credential_create(accessKey, 0, (PCHAR) "secret", 0, (PCHAR) "token", 0,
                                                  GETTIME() + 3600ULL * HUNDREDS_OF_NANOS_IN_A_SECOND, &pNew));
    aws_credential_replace(&pFake->pAwsCredentials, &pFake->pPrevAwsCredentials, pNew);
}

static void fakeSharedInit(FakeShared *pFake)
{
    MEMSET(pFake, 0, SIZEOF(*pFake));
    pFake->credentialProvider.getCredentialsFn = fakeSharedGet;
    fakeSharedRefresh(pFake);
}

static void fakeSharedFree(FakeShared *pFake)
{
    aws_credential_free(&pFake->pAwsCredentials);
    aws_credential_free(&pFake->pPrevAwsCredentials);
}

TEST_CASE("Credential view: hands out the same copy while the shared credentials are unchanged", "[credential]")
{
    FakeShared shared;
    PAwsCredentialProvider pView = NULL;
    PAwsCredentials pFirst = NULL, pAgain = NULL;

    fakeSharedInit(&shared);
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, createCredentialProviderView(&shared.credentialProvider, &pView));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pView->getCredentialsFn(pView, &pFirst));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pView->getCredentialsFn(pView, &pAgain));

    TEST_ASSERT_EQUAL_UINT32(2, shared.gets);
    TEST_ASSERT_EQUAL_PTR(pFirst, pAgain);
    /* A copy, not the shared provider's set */
    TEST_ASSERT_NOT_EQUAL(shared.pAwsCredentials, pFirst);
    TEST_ASSERT_EQUAL_STRING("AKIASHARED1", pFirst->accessKeyId);
    TEST_ASSERT_EQUAL_STRING("token", pFirst->sessionToken);
    TEST_ASSERT_EQUAL_UINT64(shared.pAwsCredentials->expiration, pFirst->expiration);

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, freeCredentialProviderView(&pView));
    TEST_ASSERT_NULL(pView);
    fakeSharedFree(&shared);
}

TEST_CASE("Credential view: its copy outlives two refreshes of the shared provider", "[credential]")
{
    FakeShared shared;
    PAwsCredentialProvider pView = NULL;
    PAwsCredentials pHeld = NULL, pNext = NULL;

    fakeSharedInit(&shared);
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, createCredentialProviderView(&shared.credentialProvider, &pView));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pView->getCredentialsFn(pView, &pHeld));

    /* Another user's refreshes free the set the view copied from */
    fakeSharedRefresh(&shared);
    fakeSharedRefresh(&shared);
    TEST_ASSERT_EQUAL_STRING("AKIASHARED1", pHeld->accessKeyId);

    /* The view's next get copies the new set and keeps the one its user may still hold */
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pView->getCredentialsFn(pView, &pNext));
    TEST_ASSERT_NOT_EQUAL(pHeld, pNext);
    TEST_ASSERT_EQUAL_STRING("AKIASHARED3", pNext->accessKeyId);
    TEST_ASSERT_EQUAL_STRING("AKIASHARED1", pHeld->accessKeyId);

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, freeCredentialProviderView(&pView));
    fakeSharedFree(&shared);
}

TEST_CASE("Credential view: after a detach a get fails without asking the shared provider", "[credential]")
{
    FakeShared shared;
    PAwsCredentialProvider pView = NULL;
    PAwsCredentials pHeld = NULL, pAfter = (PAwsCredentials) &shared;

    fakeSharedInit(&shared);
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, createCredentialProviderView(&shared.credentialProvider, &pView));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pView->getCredentialsFn(pView, &pHeld));

    detachCredentialProviderView(pView);
    /* The application may free its provider now */
    fakeSharedFree(&shared);

    TEST_ASSERT_EQUAL_HEX32(STATUS_INVALID_OPERATION, pView->getCredentialsFn(pView, &pAfter));
    TEST_ASSERT_NULL(pAfter);
    TEST_ASSERT_EQUAL_UINT32(1, shared.gets);
    /* What the view's user already holds stays valid until the view is freed */
    TEST_ASSERT_EQUAL_STRING("AKIASHARED1", pHeld->accessKeyId);

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, freeCredentialProviderView(&pView));
}

static UINT64 fakeNow(UINT64 customData)
{
    return *(PUINT64) HANDLE_TO_POINTER(customData);
}

static void writeCredentialsFile(PCHAR pPath, PCHAR pAccessKey)
{
    FILE *fp = fopen(pPath, "w");

    TEST_ASSERT_NOT_NULL(fp);
    fprintf(fp, "CREDENTIALS %s secret\n", pAccessKey);
    fclose(fp);
}

TEST_CASE("File provider: a re-read keeps the previous credentials", "[credential]")
{
    CHAR path[] = "/tmp/kvs_test_file_credentials";
    UINT64 now = GETTIME();
    PAwsCredentialProvider pProvider = NULL;
    PAwsCredentials pFirst = NULL, pSecond = NULL;

    writeCredentialsFile(path, (PCHAR) "AKIAFILE1");
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, file_credential_provider_createWithTime(path, fakeNow, POINTER_TO_HANDLE(&now), &pProvider));
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pProvider->getCredentialsFn(pProvider, &pFirst));
    TEST_ASSERT_EQUAL_STRING("AKIAFILE1", pFirst->accessKeyId);

    /* Past expiration the file is read again */
    writeCredentialsFile(path, (PCHAR) "AKIAFILE2");
    now = pFirst->expiration;
    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, pProvider->getCredentialsFn(pProvider, &pSecond));
    TEST_ASSERT_EQUAL_STRING("AKIAFILE2", pSecond->accessKeyId);
    TEST_ASSERT_EQUAL_STRING("AKIAFILE1", pFirst->accessKeyId);

    TEST_ASSERT_EQUAL_HEX32(STATUS_SUCCESS, file_credential_provider_free(&pProvider));
    remove(path);
}
