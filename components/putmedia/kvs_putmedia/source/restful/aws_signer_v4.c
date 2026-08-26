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

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Third party headers */
#include "azure_c_shared_utility/strings.h"
#include "mbedtls/build_info.h"
#include "mbedtls/md.h"
#if defined(MBEDTLS_VERSION_MAJOR) && (MBEDTLS_VERSION_MAJOR >= 4)
#include "psa/crypto.h"
#endif

/* Public headers */
#include "kvs/put_media_errors.h"

/* Internal headers */
#include "os/allocator.h"
#include "restful/aws_signer_v4.h"

#include "esp_log.h"

#define TAG "PUT_MEDIA_AWS_SIGNER"

#define HTTP_METHOD_GET "GET"
#define HTTP_METHOD_PUT "PUT"
#define HTTP_METHOD_POST "POST"

/* The buffer length used for doing SHA256 hash check. */
#define SHA256_DIGEST_LENGTH 32

/* The buffer length used for ASCII Hex encoded SHA256 result. */
#define HEX_ENCODED_SHA_256_STRING_SIZE 65

/* The string length of "date" format defined by AWS Signature V4. */
#define SIGNATURE_DATE_STRING_LEN 8

/* The signature start described by AWS Signature V4. */
#define AWS_SIG_V4_SIGNATURE_START "AWS4"

/* The signature end described by AWS Signature V4. */
#define AWS_SIG_V4_SIGNATURE_END "aws4_request"

/* The signed algorithm. */
#define AWS_SIG_V4_ALGORITHM "AWS4-HMAC-SHA256"

/* The length of oct string for SHA256 hash buffer. */
#define AWS_SIG_V4_MAX_HMAC_SIZE (SHA256_DIGEST_LENGTH * 2 + 1)

/* Template of canonical scope: <DATE>/<REGION>/<SERVICE>/<SIGNATURE_END>*/
#define TEMPLATE_CANONICAL_SCOPE "%.*s/%s/%s/%s"

/* Template of canonical signed string: <ALGO>\n<DATE_TIME>\n<SCOPE>\n<HEX_SHA_CANONICAL_REQ> */
#define TEMPLATE_CANONICAL_SIGNED_STRING "%s\n%s\n%s\n%s"

#define TEMPLATE_SIGNATURE_START "%s%s"

typedef struct AwsSigV4 {
    STRING_HANDLE xStCanonicalRequest;
    STRING_HANDLE xStSignedHeaders;
    STRING_HANDLE xStScope;
    STRING_HANDLE xStHmacHexEncoded;
    STRING_HANDLE xStAuthorization;
} AwsSigV4_t;

static int prvValidateHttpMethod(const char *pcHttpMethod)
{
    if (pcHttpMethod == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if (!strcmp(pcHttpMethod, HTTP_METHOD_POST) && !strcmp(pcHttpMethod, HTTP_METHOD_GET) && !strcmp(pcHttpMethod, HTTP_METHOD_PUT)) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        return PUT_MEDIA_ERRNO_NONE;
    }
}

static int prvValidateUri(const char *pcUri)
{
    /* TODO: Add lexical verification. */

    if (pcUri == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        return PUT_MEDIA_ERRNO_NONE;
    }
}

static int prvValidateHttpHeader(const char *pcHeader, const char *pcValue)
{
    /* TODO: Add lexical verification. */

    if (pcHeader == NULL || pcValue == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        return PUT_MEDIA_ERRNO_NONE;
    }
}

static int prvHexEncodedSha256(const unsigned char *pMsg, size_t uMsgLen, char pcHexEncodedHash[HEX_ENCODED_SHA_256_STRING_SIZE])
{
    // done for mbedtls incompatibility
//     return PUT_MEDIA_ERRNO_NONE;
// #if 0
    int res = PUT_MEDIA_ERRNO_NONE;
    int retVal = 0;
    int i = 0;
    char *p = NULL;
    unsigned char pHashBuf[SHA256_DIGEST_LENGTH] = {0};

    const mbedtls_md_info_t *pxSha256Info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (pMsg == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if (pxSha256Info == NULL) {
        res = PUT_MEDIA_ERROR_UNKNOWN_MBEDTLS_MESSAGE_DIGEST;
    }
    /* Use the generic message-digest API rather than the low-level
     * mbedtls_sha256() from mbedtls/sha256.h: mbedtls 4.x (ESP-IDF v6.x) moved
     * that header under mbedtls/private/, so the direct call no longer builds
     * there. mbedtls_md() is public across 2.x/3.x/4.x and hashes in one shot. */
    else if ((retVal = mbedtls_md(pxSha256Info, pMsg, uMsgLen, pHashBuf)) != 0) {
        res = PUT_MEDIA_GENERATE_MBEDTLS_ERROR(retVal);
    } else {
        p = pcHexEncodedHash;
        for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            p += snprintf(p, 3, "%02x", pHashBuf[i]);
        }
    }

    return res;
// #endif
}

AwsSigV4Handle AwsSigV4_Create(char *pcHttpMethod, char *pcUri, char *pcQuery)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    AwsSigV4_t *pxAwsSigV4 = NULL;

    if ((res = prvValidateHttpMethod(pcHttpMethod) == PUT_MEDIA_ERRNO_NONE && prvValidateUri(pcUri)) == PUT_MEDIA_ERRNO_NONE) {
        do {
            pxAwsSigV4 = (AwsSigV4_t *)kvsMalloc(sizeof(AwsSigV4_t));
            if (pxAwsSigV4 == NULL) {
                res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
                break;
            }
            memset(pxAwsSigV4, 0, sizeof(AwsSigV4_t));

            pxAwsSigV4->xStCanonicalRequest = STRING_construct_sprintf("%s\n%s\n%s\n", pcHttpMethod, pcUri, (pcQuery == NULL) ? "" : pcQuery);
            pxAwsSigV4->xStSignedHeaders = STRING_new();
            pxAwsSigV4->xStScope = STRING_new();
            pxAwsSigV4->xStHmacHexEncoded = STRING_new();
            pxAwsSigV4->xStAuthorization = STRING_new();

            if (pxAwsSigV4->xStCanonicalRequest == NULL || pxAwsSigV4->xStSignedHeaders == NULL || pxAwsSigV4->xStHmacHexEncoded == NULL) {
                res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
                break;
            }
        } while (0);

        if (res != PUT_MEDIA_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed to init canonical request");
            AwsSigV4_Terminate(pxAwsSigV4);
            pxAwsSigV4 = NULL;
        }
    }

    return (AwsSigV4Handle)pxAwsSigV4;
}

void AwsSigV4_Terminate(AwsSigV4Handle xSigV4Handle)
{
    AwsSigV4_t *pxAwsSigV4 = (AwsSigV4_t *)xSigV4Handle;

    if (pxAwsSigV4 != NULL) {
        STRING_delete(pxAwsSigV4->xStCanonicalRequest);
        STRING_delete(pxAwsSigV4->xStSignedHeaders);
        STRING_delete(pxAwsSigV4->xStScope);
        STRING_delete(pxAwsSigV4->xStHmacHexEncoded);
        STRING_delete(pxAwsSigV4->xStAuthorization);
        kvsFree(pxAwsSigV4);
    }
}

int AwsSigV4_AddCanonicalHeader(AwsSigV4Handle xSigV4Handle, const char *pcHeader, const char *pcValue)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    AwsSigV4_t *pxAwsSigV4 = (AwsSigV4_t *)xSigV4Handle;

    if (pxAwsSigV4 == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if ((res = prvValidateHttpHeader(pcHeader, pcValue)) != PUT_MEDIA_ERRNO_NONE) {
        /* Propagate the res error */
    } else if (STRING_sprintf(pxAwsSigV4->xStCanonicalRequest, "%s:%s\n", pcHeader, pcValue) != 0) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    } else if (STRING_length(pxAwsSigV4->xStSignedHeaders) > 0 && STRING_concat(pxAwsSigV4->xStSignedHeaders, ";") != 0) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    } else if (STRING_concat(pxAwsSigV4->xStSignedHeaders, pcHeader) != 0) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    } else {
        /* nop */
    }

    return res;
}

int AwsSigV4_AddCanonicalBody(AwsSigV4Handle xSigV4Handle, const char *pBody, size_t uBodyLen)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    AwsSigV4_t *pxAwsSigV4 = (AwsSigV4_t *)xSigV4Handle;
    char pcBodyHexEncodedSha256[HEX_ENCODED_SHA_256_STRING_SIZE] = {0};

    if (pxAwsSigV4 == NULL || pBody == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if (STRING_sprintf(pxAwsSigV4->xStCanonicalRequest, "\n%s\n", STRING_c_str(pxAwsSigV4->xStSignedHeaders)) != 0) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    } else if (prvHexEncodedSha256((const unsigned char *)pBody, uBodyLen, pcBodyHexEncodedSha256) != 0) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    } else if (STRING_concat(pxAwsSigV4->xStCanonicalRequest, pcBodyHexEncodedSha256) != 0) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    } else {
        /* nop */
    }

    return res;
}

/**
 * @brief One-shot HMAC-SHA256, portable across mbedtls 3.x and 4.x.
 *
 * mbedtls 4.x (TF-PSA-Crypto, ESP-IDF v6.x) put the whole HMAC-on-md API behind
 * MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS; PSA Crypto is the supported path there.
 * Older mbedtls keeps the classic one-shot. Output is staged through a local
 * buffer so that pMsg and pOutput may alias, which every call site below does.
 */
#if defined(MBEDTLS_VERSION_MAJOR) && (MBEDTLS_VERSION_MAJOR >= 4)
static int prvHmacSha256(const mbedtls_md_info_t *pxMdInfo, const unsigned char *pKey, size_t uKeyLen, const unsigned char *pMsg, size_t uMsgLen, unsigned char *pOutput)
{
    psa_key_attributes_t xAttributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t xKeyId = PSA_KEY_ID_NULL;
    psa_status_t xStatus = PSA_SUCCESS;
    unsigned char pMac[SHA256_DIGEST_LENGTH] = {0};
    size_t uMacLen = 0;

    (void) pxMdInfo;

    /* Idempotent, and cheap once initialised. */
    if ((xStatus = psa_crypto_init()) != PSA_SUCCESS) {
        return (int)xStatus;
    }

    psa_set_key_usage_flags(&xAttributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&xAttributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&xAttributes, PSA_KEY_TYPE_HMAC);

    if ((xStatus = psa_import_key(&xAttributes, pKey, uKeyLen, &xKeyId)) != PSA_SUCCESS) {
        return (int)xStatus;
    }

    xStatus = psa_mac_compute(xKeyId, PSA_ALG_HMAC(PSA_ALG_SHA_256), pMsg, uMsgLen, pMac, sizeof(pMac), &uMacLen);
    psa_destroy_key(xKeyId);

    if (xStatus == PSA_SUCCESS) {
        memcpy(pOutput, pMac, uMacLen);
    }

    return (int)xStatus;
}
#else
static int prvHmacSha256(const mbedtls_md_info_t *pxMdInfo, const unsigned char *pKey, size_t uKeyLen, const unsigned char *pMsg, size_t uMsgLen, unsigned char *pOutput)
{
    unsigned char pMac[SHA256_DIGEST_LENGTH] = {0};
    int retVal = mbedtls_md_hmac(pxMdInfo, pKey, uKeyLen, pMsg, uMsgLen, pMac);

    if (retVal == 0) {
        memcpy(pOutput, pMac, sizeof(pMac));
    }

    return retVal;
}
#endif

int AwsSigV4_Sign(AwsSigV4Handle xSigV4Handle, char *pcAccessKey, char *pcSecretKey, char *pcRegion, char *pcService, const char *pcXAmzDate)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    int retVal = 0;
    AwsSigV4_t *pxAwsSigV4 = (AwsSigV4_t *)xSigV4Handle;
    char pcCanonicalReqHexEncSha256[HEX_ENCODED_SHA_256_STRING_SIZE] = {0};
    const mbedtls_md_info_t *pxMdInfo = NULL;
    STRING_HANDLE xStSignedStr = NULL;
    size_t uHmacSize = 0;
    char pHmac[AWS_SIG_V4_MAX_HMAC_SIZE] = {0};
    int i = 0;

    if (pxAwsSigV4 == NULL || pcSecretKey == NULL || pcRegion == NULL || pcService == NULL || pcXAmzDate == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }
    /* Do SHA256 on canonical request and then hex encode it. */
    else if (
        (res = prvHexEncodedSha256((const unsigned char *)STRING_c_str(pxAwsSigV4->xStCanonicalRequest), STRING_length(pxAwsSigV4->xStCanonicalRequest), pcCanonicalReqHexEncSha256)) !=
        PUT_MEDIA_ERRNO_NONE) {
        /* Propagate the res error */
    } else if ((pxMdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256)) == NULL) {
        res = PUT_MEDIA_ERROR_UNKNOWN_MBEDTLS_MESSAGE_DIGEST;
    }
    /* HMAC size of SHA256 should be 32. */
    else if ((uHmacSize = mbedtls_md_get_size(pxMdInfo)) == 0) {
        res = PUT_MEDIA_ERROR_INVALID_MBEDTLS_MESSAGE_DIGEST_SIZE;
    }
    /* Generate the scope string. */
    else if (STRING_sprintf(pxAwsSigV4->xStScope, TEMPLATE_CANONICAL_SCOPE, SIGNATURE_DATE_STRING_LEN, pcXAmzDate, pcRegion, pcService, AWS_SIG_V4_SIGNATURE_END) != 0) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    }
    /* Generate the signed string. */
    else if (
        (xStSignedStr =
             STRING_construct_sprintf(TEMPLATE_CANONICAL_SIGNED_STRING, AWS_SIG_V4_ALGORITHM, pcXAmzDate, STRING_c_str(pxAwsSigV4->xStScope), pcCanonicalReqHexEncSha256)) == NULL) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    }
    /* Generate the beginning of the signature. */
    else if (snprintf(pHmac, AWS_SIG_V4_MAX_HMAC_SIZE, TEMPLATE_SIGNATURE_START, AWS_SIG_V4_SIGNATURE_START, pcSecretKey) == 0) {
        res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
    }
    /* Calculate the HMAC of date, region, service, signature end, and signed string*/
    else if (
        (retVal = prvHmacSha256(pxMdInfo, (const unsigned char *)pHmac, strlen(pHmac), (const unsigned char *)pcXAmzDate, SIGNATURE_DATE_STRING_LEN, (unsigned char *)pHmac)) != 0 ||
        (retVal = prvHmacSha256(pxMdInfo, (const unsigned char *)pHmac, uHmacSize, (const unsigned char *)pcRegion, strlen(pcRegion), (unsigned char *)pHmac)) != 0 ||
        (retVal = prvHmacSha256(pxMdInfo, (const unsigned char *)pHmac, uHmacSize, (const unsigned char *)pcService, strlen(pcService), (unsigned char *)pHmac)) != 0 ||
        (retVal = prvHmacSha256(pxMdInfo, (const unsigned char *)pHmac, uHmacSize, (const unsigned char *)AWS_SIG_V4_SIGNATURE_END, sizeof(AWS_SIG_V4_SIGNATURE_END) - 1, (unsigned char *)pHmac)) != 0 ||
        (retVal = prvHmacSha256(pxMdInfo, (const unsigned char *)pHmac, uHmacSize, (const unsigned char *)STRING_c_str(xStSignedStr), STRING_length(xStSignedStr), (unsigned char *)pHmac)) != 0) {
        res = PUT_MEDIA_GENERATE_MBEDTLS_ERROR(retVal);
    } else {
        for (i = 0; i < uHmacSize; i++) {
            if (STRING_sprintf(pxAwsSigV4->xStHmacHexEncoded, "%02x", pHmac[i] & 0xFF) != 0) {
                res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
                break;
            }
        }

        if (res == 0) {
            if (STRING_sprintf(
                        pxAwsSigV4->xStAuthorization,
                        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
                        pcAccessKey,
                        STRING_c_str(pxAwsSigV4->xStScope),
                        STRING_c_str(pxAwsSigV4->xStSignedHeaders),
                        STRING_c_str(pxAwsSigV4->xStHmacHexEncoded)) != 0) {
                res = PUT_MEDIA_ERROR_C_UTIL_STRING_ERROR;
            }
        }
    }

    STRING_delete(xStSignedStr);

    return res;
}

const char *AwsSigV4_GetAuthorization(AwsSigV4Handle xSigV4Handle)
{
    AwsSigV4_t *pxAwsSigV4 = (AwsSigV4_t *)xSigV4Handle;

    if (pxAwsSigV4 != NULL) {
        return STRING_c_str(pxAwsSigV4->xStAuthorization);
    } else {
        return NULL;
    }
}
