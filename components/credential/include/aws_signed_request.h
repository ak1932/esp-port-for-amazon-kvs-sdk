/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AWS_SIGNED_REQUEST_H__
#define __AWS_SIGNED_REQUEST_H__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_http_client.h"
#include "common_defs.h"
#include "aws_clock_skew.h"

/**
 * Called with the Date header of each response, to measure clock skew
 */
typedef VOID (*AwsServerDateFunc)(UINT64 customData, PCHAR pDate);

typedef struct {
    PCHAR pName;
    PCHAR pValue;
} AwsHttpHeader, *PAwsHttpHeader;

typedef struct {
    PCHAR pUrl;                      //!< Full https:// URL
    HTTP_REQUEST_VERB verb;
    PCHAR pBody;                     //!< NUL-terminated; NULL sends no body
    PCHAR pRegion;                   //!< Signing region
    PAwsCredentials pAwsCredentials; //!< NULL sends the request unsigned
    PCHAR pUserAgent;                //!< NULL sends DEFAULT_USER_AGENT_NAME
    PAwsHttpHeader pHeaders;         //!< Extra headers, signed with the rest. None may be one the request sets
                                     //!< itself: host, user-agent, content-type, content-length, x-amz-date,
                                     //!< x-amz-security-token, authorization
    UINT32 headerCount;
    UINT32 timeoutMs;                //!< Per attempt: a request can take (eagainRetries + 1) x timeoutMs
    UINT32 maxResponseLen;           //!< Longer responses fail the request
    UINT32 eagainRetries;            //!< Retries of a perform that returns ESP_ERR_HTTP_EAGAIN, 100 ms apart
    UINT64 clockSkew;                //!< From awsClockSkewFromDate(), applied before signing
    AwsServerDateFunc serverDateFn;  //!< Optional
    UINT64 customData;               //!< Passed to serverDateFn
    INT32 transportErrno;            //!< Out: the socket errno of a request that got no response, else 0
} AwsSignedHttpRequest, *PAwsSignedHttpRequest;

/**
 * @brief Signs a request with SigV4 (signAwsRequestInfo()) and performs it over esp_http_client,
 * TLS-verified against the certificate bundle
 *
 * The request succeeds once a response arrives, whatever its status code. Without one
 * (connect, TLS, timeout, or a response over maxResponseLen) it returns STATUS_HTTP_NO_RESPONSE
 * and leaves the socket errno in pRequest->transportErrno (0 for a response over maxResponseLen).
 * An extra header the request sets itself fails it with STATUS_INVALID_ARG.
 *
 * @param[in] pRequest The request
 * @param[out] pHttpStatus HTTP status code
 * @param[out] ppBody Optional: the NUL-terminated response body, MEMFREE() it
 * @param[out] pBodyLen Optional: its length
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS awsSignedHttpRequest(PAwsSignedHttpRequest pRequest, PUINT32 pHttpStatus, PCHAR* ppBody, PUINT32 pBodyLen);

/**
 * @brief Signs pRequestInfo and sets its headers on an esp_http_client handle, for a request whose
 * body the caller streams itself (pRequestInfo->body == NULL is signed as an empty payload)
 *
 * It only sets headers, so call it before esp_http_client_open(). A header set on the client
 * afterwards is not signed, and must not have the name of one that was.
 */
STATUS awsSignHttpClient(PRequestInfo pRequestInfo, esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif

#endif /* __AWS_SIGNED_REQUEST_H__ */
