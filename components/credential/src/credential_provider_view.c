/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_CLASS "CredentialProviderView"

#include "credential_provider_view.h"
#include "auth.h"

typedef struct {
    // First member should be the abstract credential provider
    AwsCredentialProvider credentialProvider;
    // NULL once detached
    PAwsCredentialProvider pShared;
    // The view's copy of the shared credentials, and the one before it
    PAwsCredentials pAwsCredentials;
    PAwsCredentials pPrevAwsCredentials;
    // Held across the shared get and the copy, so a detach waits for both
    MUTEX lock;
} CredentialProviderView, *PCredentialProviderView;

static BOOL prvSameString(PCHAR pA, UINT32 aLen, PCHAR pB, UINT32 bLen)
{
    if (pA == NULL || pB == NULL) {
        return pA == pB;
    }
    return aLen == bLen && MEMCMP(pA, pB, aLen) == 0;
}

/* Compared by content: a freed set's address can come back for a new one. */
static BOOL prvSameCredentials(PAwsCredentials pA, PAwsCredentials pB)
{
    return pA->expiration == pB->expiration && prvSameString(pA->accessKeyId, pA->accessKeyIdLen, pB->accessKeyId, pB->accessKeyIdLen) &&
        prvSameString(pA->secretKey, pA->secretKeyLen, pB->secretKey, pB->secretKeyLen) &&
        prvSameString(pA->sessionToken, pA->sessionTokenLen, pB->sessionToken, pB->sessionTokenLen);
}

static STATUS prvViewGet(PAwsCredentialProvider pCredentialProvider, PAwsCredentials* ppAwsCredentials)
{
    STATUS retStatus = STATUS_SUCCESS;
    PCredentialProviderView pView = (PCredentialProviderView) pCredentialProvider;
    PAwsCredentials pShared = NULL, pCopy = NULL;
    BOOL locked = FALSE;

    CHK(pView != NULL && ppAwsCredentials != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pView->lock);
    locked = TRUE;

    CHK(pView->pShared != NULL, STATUS_INVALID_OPERATION);
    CHK_STATUS(pView->pShared->getCredentialsFn(pView->pShared, &pShared));
    CHK(pShared != NULL, STATUS_NULL_ARG);

    if (pView->pAwsCredentials == NULL || !prvSameCredentials(pView->pAwsCredentials, pShared)) {
        CHK_STATUS(aws_credential_create(pShared->accessKeyId, pShared->accessKeyIdLen, pShared->secretKey, pShared->secretKeyLen,
                                         pShared->sessionToken, pShared->sessionTokenLen, pShared->expiration, &pCopy));
        aws_credential_replace(&pView->pAwsCredentials, &pView->pPrevAwsCredentials, pCopy);
    }

CleanUp:
    if (ppAwsCredentials != NULL) {
        *ppAwsCredentials = STATUS_FAILED(retStatus) ? NULL : pView->pAwsCredentials;
    }
    if (locked) {
        MUTEX_UNLOCK(pView->lock);
    }
    return retStatus;
}

STATUS createCredentialProviderView(PAwsCredentialProvider pShared, PAwsCredentialProvider* ppView)
{
    STATUS retStatus = STATUS_SUCCESS;
    PCredentialProviderView pView = NULL;

    CHK(pShared != NULL && ppView != NULL, STATUS_NULL_ARG);

    pView = (PCredentialProviderView) MEMCALLOC(1, SIZEOF(CredentialProviderView));
    CHK(pView != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pView->lock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pView->lock), STATUS_NOT_ENOUGH_MEMORY);

    pView->pShared = pShared;
    pView->credentialProvider.getCredentialsFn = prvViewGet;

CleanUp:
    if (STATUS_FAILED(retStatus)) {
        freeCredentialProviderView((PAwsCredentialProvider*) &pView);
    }
    if (ppView != NULL) {
        *ppView = (PAwsCredentialProvider) pView;
    }
    return retStatus;
}

VOID detachCredentialProviderView(PAwsCredentialProvider pCredentialProvider)
{
    PCredentialProviderView pView = (PCredentialProviderView) pCredentialProvider;

    if (pView == NULL) {
        return;
    }
    MUTEX_LOCK(pView->lock);
    pView->pShared = NULL;
    MUTEX_UNLOCK(pView->lock);
}

STATUS freeCredentialProviderView(PAwsCredentialProvider* ppView)
{
    STATUS retStatus = STATUS_SUCCESS;
    PCredentialProviderView pView = NULL;

    CHK(ppView != NULL, STATUS_NULL_ARG);

    pView = (PCredentialProviderView) *ppView;

    // Call is idempotent
    CHK(pView != NULL, retStatus);

    aws_credential_free(&pView->pAwsCredentials);
    aws_credential_free(&pView->pPrevAwsCredentials);

    if (IS_VALID_MUTEX_VALUE(pView->lock)) {
        MUTEX_FREE(pView->lock);
    }

    MEMFREE(pView);
    *ppView = NULL;

CleanUp:
    return retStatus;
}
