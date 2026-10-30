/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
/******************************************************************************
 * HEADERS
 ******************************************************************************/
#define LOG_CLASS "FileCredentialProvider"

#include "common_defs.h"
#include "platform_utils.h"
#include "auth.h"
#include "file_credential_provider.h"

/******************************************************************************
 * DEFINITIONS
 ******************************************************************************/
typedef struct __FileCredentialProvider {
    // First member should be the abstract credential provider
    AwsCredentialProvider credentialProvider;

    // Current time functionality - optional
    GetCurrentTimeFunc getCurrentTimeFn;

    // Custom data supplied to time function
    UINT64 customData;

    // Static Aws Credentials structure with the pointer following the main allocation
    PAwsCredentials pAwsCredentials;

    // The credentials before the last read, kept for a user that still holds them
    PAwsCredentials pPrevAwsCredentials;

    // Serializes get/read between the users of a shared provider
    MUTEX lock;

    // Pointer to credential file path
    PCHAR credentialsFilepath[MAX_PATH_LEN + 1];
} FileCredentialProvider, *PFileCredentialProvider;

typedef struct __CallbackCredentialProvider {
    AwsCredentialProvider credentialProvider;
    CredentialFetchCallback fetchCb;
    UINT64 customData;
    PAwsCredentials pAwsCredentials;
    // The credentials before the last fetch, kept for a user that still holds them
    PAwsCredentials pPrevAwsCredentials;
    // Serializes get/fetch between the users of a shared provider
    MUTEX lock;
} CallbackCredentialProvider, *PCallbackCredentialProvider;

/******************************************************************************
 * FUNCTIONS
 ******************************************************************************/
static STATUS priv_file_credential_provider_get(PAwsCredentialProvider, PAwsCredentials*);
static STATUS priv_file_credential_provider_read(PFileCredentialProvider);
static STATUS priv_callback_credential_provider_get(PAwsCredentialProvider, PAwsCredentials*);

STATUS file_credential_provider_create(PCHAR pCredentialsFilepath, PAwsCredentialProvider* ppCredentialProvider)
{
    return file_credential_provider_createWithTime(pCredentialsFilepath, commonDefaultGetCurrentTimeFunc, 0, ppCredentialProvider);
}

STATUS file_credential_provider_createWithTime(PCHAR pCredentialsFilepath, GetCurrentTimeFunc getCurrentTimeFn, UINT64 customData,
                                               PAwsCredentialProvider* ppCredentialProvider)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PFileCredentialProvider pFileCredentialProvider = NULL;

    CHK(ppCredentialProvider != NULL, STATUS_NULL_ARG);
    CHK(pCredentialsFilepath != NULL && pCredentialsFilepath[0] != '\0', STATUS_INVALID_ARG);
    CHK((UINT32) STRLEN(pCredentialsFilepath) <= MAX_PATH_LEN, STATUS_INVALID_ARG);

    pFileCredentialProvider = (PFileCredentialProvider) MEMCALLOC(1, SIZEOF(FileCredentialProvider));
    CHK(pFileCredentialProvider != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pFileCredentialProvider->lock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pFileCredentialProvider->lock), STATUS_NOT_ENOUGH_MEMORY);

    pFileCredentialProvider->credentialProvider.getCredentialsFn = priv_file_credential_provider_get;

    // Store the file path in case we need to access it again
    STRNCPY((PCHAR) pFileCredentialProvider->credentialsFilepath, pCredentialsFilepath, MAX_PATH_LEN);

    // Store the time functionality and specify default if NULL
    pFileCredentialProvider->getCurrentTimeFn = (getCurrentTimeFn == NULL) ? commonDefaultGetCurrentTimeFunc : getCurrentTimeFn;
    pFileCredentialProvider->customData = customData;

    // Create the credentials object
    CHK_STATUS(priv_file_credential_provider_read(pFileCredentialProvider));

CleanUp:

    if (STATUS_FAILED(retStatus)) {
        file_credential_provider_free((PAwsCredentialProvider*) &pFileCredentialProvider);
        pFileCredentialProvider = NULL;
    }

    // Set the return value if it's not NULL
    if (ppCredentialProvider != NULL) {
        *ppCredentialProvider = (PAwsCredentialProvider) pFileCredentialProvider;
    }

    LEAVES();
    return retStatus;
}

STATUS createCallbackCredentialProvider(CredentialFetchCallback fetchCb,
                                        UINT64 customData,
                                        PAwsCredentialProvider* ppCredentialProvider)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PCallbackCredentialProvider pCbProvider = NULL;

    CHK(ppCredentialProvider != NULL && fetchCb != NULL, STATUS_NULL_ARG);

    pCbProvider = (PCallbackCredentialProvider) MEMCALLOC(1, SIZEOF(CallbackCredentialProvider));
    CHK(pCbProvider != NULL, STATUS_NOT_ENOUGH_MEMORY);

    pCbProvider->lock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(pCbProvider->lock), STATUS_NOT_ENOUGH_MEMORY);

    pCbProvider->fetchCb = fetchCb;
    pCbProvider->customData = customData;
    pCbProvider->credentialProvider.getCredentialsFn = priv_callback_credential_provider_get;

CleanUp:
    if (STATUS_FAILED(retStatus)) {
        if (pCbProvider != NULL) {
            aws_credential_free(&pCbProvider->pAwsCredentials);
            if (IS_VALID_MUTEX_VALUE(pCbProvider->lock)) {
                MUTEX_FREE(pCbProvider->lock);
            }
            MEMFREE(pCbProvider);
        }
        pCbProvider = NULL;
    }
    if (ppCredentialProvider != NULL) {
        *ppCredentialProvider = (PAwsCredentialProvider) pCbProvider;
    }
    LEAVES();
    return retStatus;
}

static STATUS priv_callback_credential_provider_get(PAwsCredentialProvider pCredentialProvider, PAwsCredentials* ppAwsCredentials)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PCallbackCredentialProvider pCbProvider = (PCallbackCredentialProvider) pCredentialProvider;
    PCHAR accessKey = NULL, secretKey = NULL, sessionToken = NULL;
    UINT32 accessKeyLen = 0, secretKeyLen = 0, sessionTokenLen = 0;
    UINT64 expiration = 0;
    PAwsCredentials pNewCredentials = NULL;
    BOOL locked = FALSE;

    CHK(pCbProvider != NULL && ppAwsCredentials != NULL && pCbProvider->fetchCb != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pCbProvider->lock);
    locked = TRUE;

    // Cached until close to expiration
    CHK(pCbProvider->pAwsCredentials == NULL ||
            GETTIME() + CALLBACK_CREDENTIAL_FETCH_GRACE_PERIOD > pCbProvider->pAwsCredentials->expiration,
        retStatus);

    CHK_STATUS(pCbProvider->fetchCb(pCbProvider->customData,
                                    &accessKey, &accessKeyLen,
                                    &secretKey, &secretKeyLen,
                                    &sessionToken, &sessionTokenLen,
                                    &expiration));

    CHK_STATUS(aws_credential_create(accessKey, accessKeyLen,
                                     secretKey, secretKeyLen,
                                     sessionToken, sessionTokenLen,
                                     expiration,
                                     &pNewCredentials));

    aws_credential_replace(&pCbProvider->pAwsCredentials, &pCbProvider->pPrevAwsCredentials, pNewCredentials);

CleanUp:
    if (ppAwsCredentials != NULL) {
        *ppAwsCredentials = STATUS_FAILED(retStatus) ? NULL : pCbProvider->pAwsCredentials;
    }
    if (locked) {
        MUTEX_UNLOCK(pCbProvider->lock);
    }
    LEAVES();
    return retStatus;
}

STATUS file_credential_provider_free(PAwsCredentialProvider* ppCredentialProvider)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PFileCredentialProvider pFileCredentialProvider = NULL;

    CHK(ppCredentialProvider != NULL, STATUS_NULL_ARG);

    pFileCredentialProvider = (PFileCredentialProvider) *ppCredentialProvider;

    // Call is idempotent
    CHK(pFileCredentialProvider != NULL, retStatus);

    // Release the underlying AWS credentials objects
    aws_credential_free(&pFileCredentialProvider->pAwsCredentials);
    aws_credential_free(&pFileCredentialProvider->pPrevAwsCredentials);

    if (IS_VALID_MUTEX_VALUE(pFileCredentialProvider->lock)) {
        MUTEX_FREE(pFileCredentialProvider->lock);
    }

    // Release the object
    MEMFREE(pFileCredentialProvider);

    // Set the pointer to NULL
    *ppCredentialProvider = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}

static STATUS priv_file_credential_provider_get(PAwsCredentialProvider pCredentialProvider, PAwsCredentials* ppAwsCredentials)
{
    ENTERS();

    STATUS retStatus = STATUS_SUCCESS;
    PFileCredentialProvider pFileCredentialProvider = (PFileCredentialProvider) pCredentialProvider;
    BOOL locked = FALSE;

    CHK(pFileCredentialProvider != NULL && ppAwsCredentials != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pFileCredentialProvider->lock);
    locked = TRUE;

    // Fill the credentials
    CHK_STATUS(priv_file_credential_provider_read(pFileCredentialProvider));

    *ppAwsCredentials = pFileCredentialProvider->pAwsCredentials;

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pFileCredentialProvider->lock);
    }

    LEAVES();
    return retStatus;
}

/**
 * Read the credential file and sets the values of the AWS credentials object
 *
 * @param - PFileCredentialProvider - the PFileCredentialProvider object
 *
 * @return - STATUS code of the execution
 */

static STATUS priv_file_credential_provider_read(PFileCredentialProvider pFileCredentialProvider)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 fileLen;
    FILE* fp = NULL;
    CHAR credentialMarker[12];
    CHAR thirdTokenStr[MAX(MAX_EXPIRATION_LEN, MAX_SECRET_KEY_LEN) + 1], fourthTokenStr[MAX_SECRET_KEY_LEN + 1], accessKeyId[MAX_ACCESS_KEY_LEN + 1];
    CHAR sessionToken[MAX_SESSION_TOKEN_LEN + 1];
    PCHAR expirationStr = NULL, secretKey = NULL;
    UINT32 accessKeyIdLen = 0, secretKeyLen = 0, sessionTokenLen = 0;
    UINT64 expiration, currentTime;
    PAwsCredentials pNewCredentials = NULL;

    CHK(pFileCredentialProvider != NULL && pFileCredentialProvider->credentialsFilepath != NULL, STATUS_NULL_ARG);

    // Refresh the credentials by reading from the credentials file if needed
    currentTime = pFileCredentialProvider->getCurrentTimeFn(pFileCredentialProvider->customData);

    CHK(pFileCredentialProvider->pAwsCredentials == NULL ||
            currentTime + CREDENTIAL_FILE_READ_GRACE_PERIOD > pFileCredentialProvider->pAwsCredentials->expiration,
        retStatus);

    fp = FOPEN((PCHAR) pFileCredentialProvider->credentialsFilepath, "r");

    CHK(fp != NULL, STATUS_FILE_CREDENTIAL_PROVIDER_OPEN_FILE_FAILED);

    // Get the size of the file
    FSEEK(fp, 0, SEEK_END);

    fileLen = (UINT64) FTELL(fp);

    DLOGV("Reading AWS credentials from file: %s, file length = %" PRIu64 ".", pFileCredentialProvider->credentialsFilepath, fileLen);

    CHK(fileLen < MAX_CREDENTIAL_FILE_LEN, STATUS_FILE_CREDENTIAL_PROVIDER_INVALID_FILE_LENGTH);

    FSEEK(fp, 0, SEEK_SET);

    // empty buffers
    credentialMarker[0] = '\0';
    thirdTokenStr[0] = '\0';
    fourthTokenStr[0] = '\0';
    accessKeyId[0] = '\0';
    sessionToken[0] = '\0';

    /*
     * Currently the credential file can have two formats, one is "CREDENTIALS accessKey expiration secretKey sessionToken", and
     * the other is just "CREDENTIALS accessKey secretKey". So the second token can be either expiration or secret key.
     */

    // Both formats have at least the marker, the access key and one more token
    CHK(FSCANF(fp,
               "%11s %" STR(MAX_ACCESS_KEY_LEN) "s %" STR(MAX_EXPIRATION_LEN) "s %" STR(MAX_SECRET_KEY_LEN) "s %" STR(
                   MAX_SESSION_TOKEN_LEN) "s",
               credentialMarker, accessKeyId, thirdTokenStr, fourthTokenStr, sessionToken) >= 3,
        STATUS_FILE_CREDENTIAL_PROVIDER_INVALID_FILE_FORMAT);

    // if the fourth token is empty, it means the credential file only has accessKey and secretKey
    if (fourthTokenStr[0] == '\0') {
        secretKey = thirdTokenStr;
    } else {
        expirationStr = thirdTokenStr;
        secretKey = fourthTokenStr;
    }

    CHK(STRCMP(credentialMarker, "CREDENTIALS") == 0, STATUS_FILE_CREDENTIAL_PROVIDER_INVALID_FILE_FORMAT);

    // Set the lengths
    accessKeyIdLen = (UINT32) STRNLEN(accessKeyId, MAX_ACCESS_KEY_LEN);
    secretKeyLen = (UINT32) STRNLEN(secretKey, MAX_SECRET_KEY_LEN);
    sessionTokenLen = (UINT32) STRNLEN(sessionToken, MAX_SESSION_TOKEN_LEN);

    if (expirationStr != NULL) {
        convertTimestampToEpoch(expirationStr, currentTime / HUNDREDS_OF_NANOS_IN_A_SECOND, &expiration);
    } else {
        expiration = currentTime + MAX_ENFORCED_TOKEN_EXPIRATION_DURATION;
    }

    // Fix-up the expiration to be no more than max enforced token rotation to avoid extra token rotations
    // as we are caching the returned value which is likely to be an hour but we are enforcing max
    // rotation to be more frequent.
    expiration = MIN(expiration, currentTime + MAX_ENFORCED_TOKEN_EXPIRATION_DURATION);

    CHK(accessKeyIdLen != 0 && secretKeyLen != 0, STATUS_INVALID_AUTH_LEN);

    // Build the new credentials first: a failed create keeps the ones the provider had
    CHK_STATUS(aws_credential_create(accessKeyId, accessKeyIdLen, secretKey, secretKeyLen, sessionToken, sessionTokenLen, expiration,
                                     &pNewCredentials));
    aws_credential_replace(&pFileCredentialProvider->pAwsCredentials, &pFileCredentialProvider->pPrevAwsCredentials, pNewCredentials);

CleanUp:

    if (fp != NULL) {
        FCLOSE(fp);
    }

    return retStatus;
}

STATUS freeCallbackCredentialProvider(PAwsCredentialProvider* ppCredentialProvider)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PCallbackCredentialProvider pCbProvider = NULL;

    CHK(ppCredentialProvider != NULL, STATUS_NULL_ARG);

    pCbProvider = (PCallbackCredentialProvider) *ppCredentialProvider;

    // Call is idempotent
    CHK(pCbProvider != NULL, retStatus);

    // Release the underlying AWS credentials objects
    aws_credential_free(&pCbProvider->pAwsCredentials);
    aws_credential_free(&pCbProvider->pPrevAwsCredentials);

    if (IS_VALID_MUTEX_VALUE(pCbProvider->lock)) {
        MUTEX_FREE(pCbProvider->lock);
    }

    // Release the object
    MEMFREE(pCbProvider);

    // Set the pointer to NULL
    *ppCredentialProvider = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}
