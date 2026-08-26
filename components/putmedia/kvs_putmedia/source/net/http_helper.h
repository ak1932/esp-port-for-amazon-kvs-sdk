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

#ifndef HTTP_HELPER_H
#define HTTP_HELPER_H

#include "kvs/put_media_errors.h"
#include <stddef.h>

#define HTTP_METHOD_GET_STR "GET"
#define HTTP_METHOD_POST_STR "POST"

#include "azure_c_shared_utility/httpheaders.h"
#include "esp_http_client.h"

#define URI_QUERY_EMPTY                 ""

/* Headers that needs to be signed in AWS signer V4 */
#define HDR_CONNECTION                  "connection"
#define HDR_HOST                        "host"
#define HDR_TRANSFER_ENCODING           "transfer-encoding"
#define HDR_USER_AGENT                  "user-agent"
#define HDR_X_AMZ_DATE                  "x-amz-date"
#define HDR_X_AMZ_SECURITY_TOKEN        "x-amz-security-token"
#define HDR_X_AMZN_FRAG_ACK_REQUIRED    "x-amzn-fragment-acknowledgment-required"
#define HDR_X_AMZN_FRAG_T_TYPE          "x-amzn-fragment-timecode-type"
#define HDR_X_AMZN_IOT_THINGNAME        "x-amzn-iot-thingname"
#define HDR_X_AMZN_PRODUCER_START_T     "x-amzn-producer-start-timestamp"
#define HDR_X_AMZN_STREAM_NAME          "x-amzn-stream-name"

/* Headers that doesn't need sign */
#define HDR_ACCEPT                      "accept"
#define HDR_AUTHORIZATION               "authorization"
#define HDR_CONTENT_LENGTH              "content-length"
#define HDR_CONTENT_TYPE                "content-type"

#define VAL_ACCEPT_ANY                  "*/*"
#define VAL_CONTENT_TYPE_APPLICATION_jSON   "application/json"
#define VAL_USER_AGENT                  "myagent"
#define VAL_KEEP_ALIVE                  "keep-alive"
#define VAL_TRANSFER_ENCODING_CHUNKED   "chunked"
#define VAL_FRAGMENT_ACK_REQUIRED_TRUE  "1"

#define HTTP_BODY_EMPTY                 ""

/**
 * @brief Perform HTTP request using esp_http_client (one-shot, no client cert).
 *
 * @param[in] pcHost Hostname
 * @param[in] pcPort Port (e.g. "443")
 * @param[in] pcUri URI path (e.g. "/createStream")
 * @param[in] pcHttpMethod HTTP method (GET, POST)
 * @param[in] xHttpReqHeaders HTTP request headers
 * @param[in] pcBody Request body (use HTTP_BODY_EMPTY for none)
 * @param[in] uRecvTimeoutMs Receive timeout in ms
 * @param[in] uSendTimeoutMs Send timeout in ms
 * @param[out] puHttpStatus HTTP status code
 * @param[out] ppRspBody Response body (caller must kvsFree); may be NULL if not needed
 * @param[out] puRspBodyLen Length of response body
 * @return 0 on success, non-zero put_media error otherwise
 */
int Http_performRequest(const char *pcHost, const char *pcPort, const char *pcUri,
                        esp_http_client_method_t http_method, HTTP_HEADERS_HANDLE xHttpReqHeaders,
                        const char *pcBody, unsigned int uRecvTimeoutMs, unsigned int uSendTimeoutMs,
                        unsigned int *puHttpStatus, char **ppRspBody, size_t *puRspBodyLen);

/**
 * @brief Perform HTTP request with X.509 client certificate (e.g. IoT credentials).
 *
 * @param[in] pcHost Hostname
 * @param[in] pcPort Port (e.g. "443")
 * @param[in] pcUri URI path
 * @param[in] pcHttpMethod HTTP method
 * @param[in] xHttpReqHeaders HTTP request headers
 * @param[in] pcBody Request body
 * @param[in] uRecvTimeoutMs Receive timeout in ms
 * @param[in] uSendTimeoutMs Send timeout in ms
 * @param[in] pcRootCA Server root CA PEM (NULL to skip verification)
 * @param[in] pcCertPem Client certificate PEM
 * @param[in] pcKeyPem Client private key PEM
 * @param[out] puHttpStatus HTTP status code
 * @param[out] ppRspBody Response body (caller must kvsFree)
 * @param[out] puRspBodyLen Length of response body
 * @return 0 on success, non-zero put_media error otherwise
 */
put_media_err_t Http_performRequestWithX509(const char *pcHost, const char *pcPort, const char *pcUri,
                                            esp_http_client_method_t http_method, HTTP_HEADERS_HANDLE xHttpReqHeaders,
                                            const char *pcBody, unsigned int uRecvTimeoutMs, unsigned int uSendTimeoutMs,
                                            const char *pcRootCA, const char *pcCertPem, const char *pcKeyPem,
                                            unsigned int *puHttpStatus, char **ppRspBody, size_t *puRspBodyLen);

/**
 * @brief Open HTTP connection for chunked upload (Put Media streaming).
 * Sends request line and headers; does not send body. Caller uses Http_write_chunk()
 * (esp_http_client_write_chunk) for each body chunk. Call esp_http_client_cleanup when done.
 *
 * @param[in] pcHost Hostname
 * @param[in] pcPort Port (e.g. "443")
 * @param[in] pcUri URI path (e.g. "/putMedia")
 * @param[in] xHttpReqHeaders HTTP request headers
 * @param[in] uRecvTimeoutMs Receive timeout in ms
 * @param[in] uSendTimeoutMs Send timeout in ms
 * @param[out] puHttpStatus Initial response status (100 or 200)
 * @param[out] ppClient esp_http_client handle (caller must esp_http_client_cleanup)
 * @return 0 on success, non-zero put_media error otherwise
 */
put_media_err_t Http_openChunkedRequest(const char *pcHost, const char *pcPort, const char *pcUri,
                                        HTTP_HEADERS_HANDLE xHttpReqHeaders,
                                        unsigned int uRecvTimeoutMs, unsigned int uSendTimeoutMs,
                                        unsigned int *puHttpStatus, esp_http_client_handle_t *ppClient);

/**
 * @brief Send http chunk over esp_http_client
 *
 * @param[in] http_client esp_http_client handle
 * @param[in] pBuffer Chunk buffer to write
 * @param[in] uBytesToSend Bytes to write
 * @return 0 on success, non-zero put_media error otherwise
 */
put_media_err_t Http_write_chunk(esp_http_client_handle_t http_client, const char *pBuffer, size_t uBytesToSend);
#endif /* HTTP_HELPER_H */
