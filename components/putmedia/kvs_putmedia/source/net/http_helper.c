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

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Public headers */
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "kvs/put_media_errors.h"

/* Internal headers */
#include "os/allocator.h"
#include "net/http_helper.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_idf_version.h"

#include "esp_heap_caps.h"

#define TAG "PUT_MEDIA_HTTP"

/*
 * esp_http_client_chunk_write_begin() / _end() landed after v6.0.2 - v6.1 is the
 * first release carrying them. Older IDFs get the hand-rolled framing below, so
 * this component still builds on v5.4 / v5.5
 */
#ifndef HTTP_USE_NATIVE_CHUNK_API
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
#define HTTP_USE_NATIVE_CHUNK_API 1
#else
#define HTTP_USE_NATIVE_CHUNK_API 0
#endif
#endif

/* Amazon Root CA 1 - used for *.amazonaws.com server verification when esp_crt_bundle is not available */

#define DEFAULT_HTTP_RECV_BUFSIZE  2048
#define MAX_HTTP_RESPONSE_BODY     (64 * 1024)
#define URL_MAX_LEN                512
/* KVS requests have many headers (Host, Authorization, x-amz-*, etc.); need larger
 * than the 512-byte default. Split TX from RX rather than paying 4 KB twice per
 * client: esp_http_client takes both from the ordinary heap, and on a part where
 * malloc() is internal-only that is 8 KB of the scarcest pool for each of the
 * three clients this file opens.
 *
 * TX has to hold the whole request head in one buffer - the SigV4 Authorization
 * header plus a ~1.5 KB x-amz-security-token when STS credentials are in use - so
 * it keeps the full 4 KB. RX only ever reads response headers and short JSON
 * bodies (the body itself is accumulated separately, see MAX_HTTP_RESPONSE_BODY),
 * so 2 KB is ample. */
#define HTTP_CLIENT_TX_BUFFER_SIZE 4096
#define HTTP_CLIENT_RX_BUFFER_SIZE 2048

typedef struct {
    char *pBuf;
    size_t uCur;
    size_t uCap;
} HttpRespCtx_t;

static esp_err_t prvHttpEventHandler(esp_http_client_event_t *pEvt)
{
    HttpRespCtx_t *pCtx = (HttpRespCtx_t *)pEvt->user_data;

    if (pCtx == NULL) {
        return ESP_OK;
    }
    switch (pEvt->event_id) {
    case HTTP_EVENT_ON_DATA: {
        if (pEvt->data == NULL || pEvt->data_len <= 0) {
            break;
        }

        ESP_LOGD(TAG, "response len: %d", pEvt->data_len);

        size_t need = pCtx->uCur + (size_t)pEvt->data_len + 1;
        if (need > pCtx->uCap) {
            if (need > MAX_HTTP_RESPONSE_BODY) {
                return ESP_FAIL;
            }
            size_t new_cap = pCtx->uCap == 0 ? DEFAULT_HTTP_RECV_BUFSIZE : pCtx->uCap * 2;
            while (new_cap < need) {
                new_cap *= 2;
            }
            if (new_cap > MAX_HTTP_RESPONSE_BODY) {
                new_cap = MAX_HTTP_RESPONSE_BODY;
            }
            char *pNew = (char *)kvsMalloc(new_cap);
            if (pNew == NULL) {
                return ESP_FAIL;
            }
            if (pCtx->pBuf != NULL) {
                memcpy(pNew, pCtx->pBuf, pCtx->uCur);
                kvsFree(pCtx->pBuf);
            }
            pCtx->pBuf = pNew;
            pCtx->uCap = new_cap;
        }
        memcpy(pCtx->pBuf + pCtx->uCur, pEvt->data, (size_t)pEvt->data_len);
        pCtx->uCur += (size_t)pEvt->data_len;
        pCtx->pBuf[pCtx->uCur] = '\0';

        break;
    }

    default: {
        ESP_LOGD(TAG, "Event %d not handled", pEvt->event_id);
        break;
    }
    }
    return ESP_OK;
}

static int prvSetHeadersFromHandle(esp_http_client_handle_t h, HttpHeaders_t *xHttpReqHeaders)
{
    size_t uHeadersCnt = HttpHeaders_count(xHttpReqHeaders);
    const char *pcName = NULL;
    const char *pcValue = NULL;
    size_t i;

    for (i = 0; i < uHeadersCnt; i++) {
        if (HttpHeaders_get(xHttpReqHeaders, i, &pcName, &pcValue) != PUT_MEDIA_ERRNO_NONE) {
            return PUT_MEDIA_ERROR_UNABLE_TO_GET_HTTP_HEADER;
        }
        /* Never log the SigV4 signature or the STS session token. */
        bool bSecret = (strcasecmp(pcName, HDR_AUTHORIZATION) == 0 ||
                        strcasecmp(pcName, HDR_X_AMZ_SECURITY_TOKEN) == 0);
        ESP_LOGD(TAG, "%s: %s", pcName, bSecret ? "<redacted>" : pcValue);
        if (esp_http_client_set_header(h, pcName, pcValue) != ESP_OK) {
            return PUT_MEDIA_ERROR_UNABLE_TO_GET_HTTP_HEADER;
        }
    }
    return PUT_MEDIA_ERRNO_NONE;
}

static int prvMapEspErrToPutMedia(esp_err_t err)
{
    if (err == ESP_OK) {
        return PUT_MEDIA_ERRNO_NONE;
    }
    if (err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_CONNECTING) {
        return PUT_MEDIA_ERROR_RECV_ZERO_SIZED_HTTP_DATA; /* connection failure */
    }
    if (err == ESP_ERR_HTTP_READ_TIMEOUT) {
        return PUT_MEDIA_ERROR_RECV_ZERO_SIZED_HTTP_DATA;
    }
    return PUT_MEDIA_ERROR_RECV_ZERO_SIZED_HTTP_DATA;
}

int Http_performRequest(const char *pcHost, const char *pcPort, const char *pcUri,
                        esp_http_client_method_t http_method, HttpHeaders_t *xHttpReqHeaders,
                        const char *pcBody, unsigned int uRecvTimeoutMs, unsigned int uSendTimeoutMs,
                        unsigned int *puHttpStatus, char **ppRspBody, size_t *puRspBodyLen)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    char url[URL_MAX_LEN];
    esp_http_client_config_t cfg = { 0 };
    esp_http_client_handle_t h = NULL;
    HttpRespCtx_t ctx = { 0 };
    esp_err_t err;
    int status;

    if (pcHost == NULL || pcPort == NULL || pcUri == NULL  ||
            xHttpReqHeaders == NULL || pcBody == NULL || puHttpStatus == NULL || ppRspBody == NULL || puRspBodyLen == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

    *puHttpStatus = 0;
    *ppRspBody = NULL;
    *puRspBodyLen = 0;

    snprintf(url, sizeof(url), "https://%s:%s%s", pcHost, pcPort, pcUri);

    cfg.url = url;
    cfg.method = http_method;
    cfg.timeout_ms = (int)uRecvTimeoutMs;
    cfg.event_handler = prvHttpEventHandler;
    cfg.user_data = &ctx;
    // cfg.cert_pem = s_amazon_root_ca_1_pem;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // cfg.cert_pem = server_cert;
    cfg.buffer_size = (int)HTTP_CLIENT_RX_BUFFER_SIZE;
    cfg.buffer_size_tx = (int)HTTP_CLIENT_TX_BUFFER_SIZE;

    h = esp_http_client_init(&cfg);
    if (h == NULL) {
        return PUT_MEDIA_ERROR_FAIL_TO_CREATE_NETIO_HANDLE;
    }

    esp_http_client_set_timeout_ms(h, uRecvTimeoutMs);

    res = prvSetHeadersFromHandle(h, xHttpReqHeaders);
    if (res != PUT_MEDIA_ERRNO_NONE) {
        esp_http_client_cleanup(h);
        return res;
    }

    if (http_method == HTTP_METHOD_POST && strlen(pcBody) > 0) {
        ESP_LOGD(TAG, "body: %s", pcBody);
        esp_http_client_set_post_field(h, pcBody, strlen(pcBody));
    }

    err = esp_http_client_perform(h);
    if (err != ESP_OK) {
        res = prvMapEspErrToPutMedia(err);
        esp_http_client_cleanup(h);
        return res;
    }

    status = esp_http_client_get_status_code(h);
    *puHttpStatus = (unsigned int)status;

    if (ctx.pBuf != NULL && ctx.uCur > 0) {
        *ppRspBody = (char *)kvsMalloc(ctx.uCur + 1);
        if (*ppRspBody == NULL) {
            res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
        } else {
            memcpy(*ppRspBody, ctx.pBuf, ctx.uCur);
            (*ppRspBody)[ctx.uCur] = '\0';
            *puRspBodyLen = ctx.uCur;
        }
        kvsFree(ctx.pBuf);
        ctx.pBuf = NULL;
    }

    esp_http_client_cleanup(h);
    return res;
}

put_media_err_t Http_performRequestWithX509(const char *pcHost, const char *pcPort, const char *pcUri,
                                            esp_http_client_method_t http_method, HttpHeaders_t *xHttpReqHeaders,
                                            const char *pcBody, unsigned int uRecvTimeoutMs, unsigned int uSendTimeoutMs,
                                            const char *pcRootCA, const char *pcCertPem, const char *pcKeyPem,
                                            unsigned int *puHttpStatus, char **ppRspBody, size_t *puRspBodyLen)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    char url[URL_MAX_LEN];
    esp_http_client_config_t cfg = { 0 };
    esp_http_client_handle_t h = NULL;
    HttpRespCtx_t ctx = { 0 };
    esp_err_t err;
    int status;

    if (pcHost == NULL || pcPort == NULL || pcUri == NULL ||
            xHttpReqHeaders == NULL || pcBody == NULL || pcCertPem == NULL || pcKeyPem == NULL ||
            puHttpStatus == NULL || ppRspBody == NULL || puRspBodyLen == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

    *puHttpStatus = 0;
    *ppRspBody = NULL;
    *puRspBodyLen = 0;

    snprintf(url, sizeof(url), "https://%s:%s%s", pcHost, pcPort, pcUri);

    cfg.url = url;
    cfg.method = http_method;
    cfg.timeout_ms = (int)uRecvTimeoutMs;
    cfg.event_handler = prvHttpEventHandler;
    cfg.user_data = &ctx;
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.client_cert_pem = pcCertPem;
    cfg.client_key_pem = pcKeyPem;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // if (pcRootCA != NULL && strlen(pcRootCA) > 0) {
    //     cfg.cert_pem = pcRootCA;
    // } else {
    //     cfg.cert_pem = s_amazon_root_ca_1_pem;
    // }
    cfg.buffer_size = (int)HTTP_CLIENT_RX_BUFFER_SIZE;
    cfg.buffer_size_tx = (int)HTTP_CLIENT_TX_BUFFER_SIZE;

    h = esp_http_client_init(&cfg);
    if (h == NULL) {
        return PUT_MEDIA_ERROR_FAIL_TO_CREATE_NETIO_HANDLE;
    }

    esp_http_client_set_timeout_ms(h, uRecvTimeoutMs);

    res = prvSetHeadersFromHandle(h, xHttpReqHeaders);
    if (res != PUT_MEDIA_ERRNO_NONE) {
        esp_http_client_cleanup(h);
        return res;
    }

    if (http_method == HTTP_METHOD_POST && strlen(pcBody) > 0) {
        esp_http_client_set_post_field(h, pcBody, strlen(pcBody));
    }

    err = esp_http_client_perform(h);
    if (err != ESP_OK) {
        res = prvMapEspErrToPutMedia(err);
        esp_http_client_cleanup(h);
        return res;
    }

    status = esp_http_client_get_status_code(h);
    *puHttpStatus = (unsigned int)status;

    if (ctx.pBuf != NULL && ctx.uCur > 0) {
        *ppRspBody = (char *)kvsMalloc(ctx.uCur + 1);
        if (*ppRspBody == NULL) {
            res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
        } else {
            memcpy(*ppRspBody, ctx.pBuf, ctx.uCur);
            (*ppRspBody)[ctx.uCur] = '\0';
            *puRspBodyLen = ctx.uCur;
        }
        kvsFree(ctx.pBuf);
        ctx.pBuf = NULL;
    }

    esp_http_client_cleanup(h);
    return res;
}

/*
 * Emit one complete chunk: size line, payload, trailer.
 *
 * Two implementations, chosen by IDF version:
 *
 *   native (6.1+)  esp_http_client_chunk_write_begin()/_end(). No allocation and
 *                  no copy of the payload; three transport writes per chunk.
 *   hand-framed    one buffer, one transport write; costs one malloc and one copy
 *                  of the payload per chunk. Keeps the component building on v5.4
 *                  and v5.5, where the chunk APIs do not exist.
 */
#if HTTP_USE_NATIVE_CHUNK_API

/*
 * Whether the chunk most recently begun still owes its trailer.
 *
 * esp_http_client_chunk_write_end(client, true) is meant to close the LAST chunk
 * - it emits that chunk's "\r\n" and the "0\r\n\r\n" terminator together, and
 * its documented precondition is that a chunk was started with
 * chunk_write_begin(). Calling it standalone after every chunk has closed itself
 * would emit a trailer for a chunk that is not open, leaving a blank line where
 * the parser expects the last-chunk size.
 */
static bool s_bChunkTrailerPending;

/* Emit the deferred "\r\n" for the chunk still open, and the end-of-body
 * terminator with it when this closes the last one. No-op when nothing is open. */
static put_media_err_t prvCloseOpenChunk(esp_http_client_handle_t http_client, bool bLastChunk)
{
    if (!s_bChunkTrailerPending) {
        return PUT_MEDIA_ERRNO_NONE;
    }

    int nRes = esp_http_client_chunk_write_end(http_client, bLastChunk);

    /* Cleared either way: on failure the body is unusable and the session is
     * going away, and the next open must not inherit this. */
    s_bChunkTrailerPending = false;

    if (nRes != 0) {
        ESP_LOGE(TAG, "Chunk trailer failed, HTTP stream is no longer usable");
        return PUT_MEDIA_ERROR_NETIO_UNABLE_TO_SEND_CONTENT;
    }

    return PUT_MEDIA_ERRNO_NONE;
}

static put_media_err_t prvWriteFramedChunk(esp_http_client_handle_t http_client, const char *pBuffer, size_t uPayload)
{
    /* Close the previous chunk - its trailer was held back in case it turned out
     * to be the last one. It did not. */
    put_media_err_t err = prvCloseOpenChunk(http_client, false);
    if (err != PUT_MEDIA_ERRNO_NONE) {
        return err;
    }

    int nRes = esp_http_client_chunk_write_begin(http_client, (int)uPayload);

    if (nRes != 0) {
        ESP_LOGE(TAG, "Chunk size line failed for %zu bytes, HTTP stream is no longer usable", uPayload);
        return PUT_MEDIA_ERROR_NETIO_UNABLE_TO_SEND_CONTENT;
    }

    s_bChunkTrailerPending = true;

    int nWritten = esp_http_client_write(http_client, pBuffer, (int)uPayload);

    if (nWritten != (int)uPayload) {
        /* The size line is already out, so this body is short by an unknown
         * amount and the stream cannot be repaired. */
        ESP_LOGE(TAG, "Chunk body wrote %d of %zu bytes, HTTP stream is no longer usable", nWritten, uPayload);
        return PUT_MEDIA_ERROR_NETIO_UNABLE_TO_SEND_CONTENT;
    }

    return PUT_MEDIA_ERRNO_NONE;
}

#else /* !HTTP_USE_NATIVE_CHUNK_API */

static put_media_err_t prvWriteFramedChunk(esp_http_client_handle_t http_client, const char *pBuffer, size_t uPayload)
{
    char pcSizeLine[20];

    int nSizeLen = snprintf(pcSizeLine, sizeof(pcSizeLine), "%zx\r\n", uPayload);
    if (nSizeLen <= 0 || (size_t)nSizeLen >= sizeof(pcSizeLine)) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

    size_t uFramed = (size_t)nSizeLen + uPayload + 2;
    char *pcFrame = heap_caps_malloc_prefer(uFramed, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
    if (pcFrame == NULL) {
        ESP_LOGE(TAG, "No memory to frame a %zu byte chunk", uPayload);
        return PUT_MEDIA_ERROR_OUT_OF_MEMORY;
    }

    memcpy(pcFrame, pcSizeLine, (size_t)nSizeLen);
    memcpy(pcFrame + nSizeLen, pBuffer, uPayload);
    memcpy(pcFrame + nSizeLen + uPayload, "\r\n", 2);

    int nWritten = esp_http_client_write(http_client, pcFrame, (int)uFramed);

    free(pcFrame);

    if (nWritten != (int)uFramed) {
        ESP_LOGE(TAG, "Chunk wrote %d of %zu bytes, HTTP stream is no longer usable", nWritten, uFramed);
        return PUT_MEDIA_ERROR_NETIO_UNABLE_TO_SEND_CONTENT;
    }

    return PUT_MEDIA_ERRNO_NONE;
}

#endif /* HTTP_USE_NATIVE_CHUNK_API */

put_media_err_t Http_write_chunk(esp_http_client_handle_t http_client, const char *pBuffer, size_t uBytesToSend)
{
    if (http_client == NULL || pBuffer == NULL || uBytesToSend == 0 || uBytesToSend > INT_MAX) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

    return prvWriteFramedChunk(http_client, pBuffer, uBytesToSend);
}

put_media_err_t Http_finishChunkedRequest(esp_http_client_handle_t http_client)
{
    if (http_client == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

#if HTTP_USE_NATIVE_CHUNK_API
    /* The last chunk is still open, holding its trailer for exactly this: one
     * call closes it and emits the terminator, which is the sequence
     * esp_http_client_chunk_write_end() is built for. */
    if (s_bChunkTrailerPending) {
        return prvCloseOpenChunk(http_client, true);
    }

    /* No chunk was ever written on this connection, so there is nothing for
     * chunk_write_end() to close and its precondition is not met. The body is
     * just the terminator. */
#endif

    static const char terminator[] = "0\r\n\r\n";
    const int nLen = (int)(sizeof(terminator) - 1);

    if (esp_http_client_write(http_client, terminator, nLen) != nLen) {
        ESP_LOGE(TAG, "Failed to write the end-of-body terminator");
        return PUT_MEDIA_ERROR_NETIO_UNABLE_TO_SEND_CONTENT;
    }

    return PUT_MEDIA_ERRNO_NONE;
}

put_media_err_t Http_openChunkedRequest(const char *pcHost, const char *pcPort, const char *pcUri,
                                        HttpHeaders_t *xHttpReqHeaders,
                                        unsigned int uRecvTimeoutMs, unsigned int uSendTimeoutMs,
                                        unsigned int *puHttpStatus, esp_http_client_handle_t *ppClient)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    char url[URL_MAX_LEN];
    esp_http_client_config_t cfg = { 0 };
    esp_http_client_handle_t h = NULL;
    esp_err_t err;
    int status;

#if HTTP_USE_NATIVE_CHUNK_API
    /* A previous session may have been torn down mid-chunk; this one starts
     * owing nothing. */
    s_bChunkTrailerPending = false;
#endif

    if (pcHost == NULL || pcPort == NULL || pcUri == NULL || xHttpReqHeaders == NULL ||
            puHttpStatus == NULL || ppClient == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

    *puHttpStatus = 0;
    *ppClient = NULL;

    snprintf(url, sizeof(url), "https://%s:%s%s", pcHost, pcPort, pcUri);

    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = (int)uRecvTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = (int)HTTP_CLIENT_RX_BUFFER_SIZE;
    cfg.buffer_size_tx = (int)HTTP_CLIENT_TX_BUFFER_SIZE;
    cfg.event_handler = prvHttpEventHandler;

    h = esp_http_client_init(&cfg);
    if (h == NULL) {
        return PUT_MEDIA_ERROR_FAIL_TO_CREATE_NETIO_HANDLE;
    }

    esp_http_client_set_timeout_ms(h, uRecvTimeoutMs);

    res = prvSetHeadersFromHandle(h, xHttpReqHeaders);
    if (res != PUT_MEDIA_ERRNO_NONE) {
        esp_http_client_cleanup(h);
        return res;
    }

    err = esp_http_client_open(h, -1);
    if (err != ESP_OK) {
        res = prvMapEspErrToPutMedia(err);
        esp_http_client_cleanup(h);
        return res;
    }

    /*
     * esp_http_client_fetch_headers() returns the content length (0 when the
     * response carries no Content-Length), not an esp_err_t. Only a negative
     * value is an error.
     */
    if (esp_http_client_fetch_headers(h) < 0) {
        res = PUT_MEDIA_ERROR_RECV_ZERO_SIZED_HTTP_DATA;
        esp_http_client_cleanup(h);
        return res;
    }

    status = esp_http_client_get_status_code(h);
    *puHttpStatus = (unsigned int)status;
    *ppClient = h;
    return PUT_MEDIA_ERRNO_NONE;
}
