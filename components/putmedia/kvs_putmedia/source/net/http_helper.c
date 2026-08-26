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
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Third party headers */
#include "azure_c_shared_utility/httpheaders.h"

/* Public headers */
#include "azure_c_shared_utility/strings.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "kvs/put_media_errors.h"

/* Internal headers */
#include "os/allocator.h"
#include "net/http_helper.h"

#include "esp_http_client.h"
#include "esp_log.h"

#define TAG "PUT_MEDIA_HTTP"

/* Amazon Root CA 1 - used for *.amazonaws.com server verification when esp_crt_bundle is not available */

#define DEFAULT_HTTP_RECV_BUFSIZE  2048
#define MAX_HTTP_RESPONSE_BODY     (64 * 1024)
#define URL_MAX_LEN                512
/* KVS requests have many headers (Host, Authorization, x-amz-*, etc.); need larger than default 512 */
#define HTTP_CLIENT_BUFFER_SIZE    4096

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

        ESP_LOGI(TAG, "response len: %d", pEvt->data_len);

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
        ESP_LOGW(TAG, "Event %d not handled", pEvt->event_id);
        break;
    }
    }
    return ESP_OK;
}

/* Parse "name: value" into key and value; value may have leading space after colon. */
static void prvParseHeaderLine(const char *pcLine, char *pcKey, size_t uKeySize, char *pcVal, size_t uValSize)
{
    const char *pColon = strchr(pcLine, ':');
    if (pColon == NULL) {
        if (pcKey && uKeySize) {
            pcKey[0] = '\0';
        }
        if (pcVal && uValSize) {
            pcVal[0] = '\0';
        }
        return;
    }
    size_t keyLen = (size_t)(pColon - pcLine);
    if (keyLen >= uKeySize) {
        ESP_LOGE(TAG, "Value buf size: %d, value required size: %d to parse HTTP Headers", uKeySize, keyLen);
        keyLen = uKeySize - 1;
    }
    memcpy(pcKey, pcLine, keyLen);
    pcKey[keyLen] = '\0';
    const char *pVal = pColon + 1;
    while (*pVal == ' ' || *pVal == '\t') {
        pVal++;
    }
    size_t valLen = strlen(pVal);
    if (valLen >= uValSize) {
        ESP_LOGE(TAG, "Value buf size: %d, value required size: %d to parse HTTP Headers", uValSize, valLen);
        valLen = uValSize - 1;
    }
    memcpy(pcVal, pVal, valLen);
    pcVal[valLen] = '\0';
}

static int prvSetHeadersFromHandle(esp_http_client_handle_t h, HTTP_HEADERS_HANDLE xHttpReqHeaders)
{
    size_t uHeadersCnt = 0;
    size_t i;
    char *pcHeader = NULL;
    char key[128];
    char value[1024];

    if (HTTPHeaders_GetHeaderCount(xHttpReqHeaders, &uHeadersCnt) != HTTP_HEADERS_OK) {
        return PUT_MEDIA_ERROR_UNABLE_TO_GET_HTTP_HEADER_COUNT;
    }
    for (i = 0; i < uHeadersCnt; i++) {
        if (HTTPHeaders_GetHeader(xHttpReqHeaders, i, &pcHeader) != HTTP_HEADERS_OK) {
            return PUT_MEDIA_ERROR_UNABLE_TO_GET_HTTP_HEADER;
        }
        prvParseHeaderLine(pcHeader, key, sizeof(key), value, sizeof(value));
        free(pcHeader);
        pcHeader = NULL;
        if (key[0] != '\0') {
            ESP_LOGI(TAG, "%s: %s", key, value);
            esp_err_t err = esp_http_client_set_header(h, key, value);
            if (err != ESP_OK) {
                return PUT_MEDIA_ERROR_UNABLE_TO_GET_HTTP_HEADER;
            }
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

//
// int replace_fn(unsigned int uRecvTimeoutMs, unsigned int uSendTimeoutMs, const char *pcHost, const char *pcPort) {
//     NetIoHandle xNetIoHandle;
//     put_media_err_t res = PUT_MEDIA_ERRNO_NONE;
//     if ((xNetIoHandle = NetIo_create()) == NULL) {
//         res = PUT_MEDIA_ERROR_FAIL_TO_CREATE_NETIO_HANDLE;
//         ESP_LOGE(TAG, "Failed to create NetIo handle");
//     } else if (
//         (res = NetIo_setRecvTimeout(xNetIoHandle, uRecvTimeoutMs)) != PUT_MEDIA_ERRNO_NONE ||
//         (res = NetIo_setSendTimeout(xNetIoHandle, uSendTimeoutMs)) != PUT_MEDIA_ERRNO_NONE ||
//         (res = NetIo_connect(xNetIoHandle, pcHost, PORT_HTTPS)) != PUT_MEDIA_ERRNO_NONE) {
//         ESP_LOGE(TAG, "Failed to connect to %s", pcHost);
//         /* Propagate the res error */
//     } else if ((res = Http_executeHttpReq(xNetIoHandle, HTTP_METHOD_POST, KVS_URI_CREATE_STREAM, xHttpReqHeaders, STRING_c_str(xStHttpBody))) != PUT_MEDIA_ERRNO_NONE) {
//         ESP_LOGE(TAG, "Failed send http request to %s", pcHost);
//         /* Propagate the res error */
//     } else if ((res = Http_recvHttpRsp(xNetIoHandle, &uHttpStatusCode, &pRspBody, &uRspBodyLen)) != PUT_MEDIA_ERRNO_NONE) {
//         ESP_LOGE(TAG, "Failed recv http response from %s", pServPara->pcHost);
//         /* Propagate the res error */
//     }
// }

int Http_performRequest(const char *pcHost, const char *pcPort, const char *pcUri,
                        esp_http_client_method_t http_method, HTTP_HEADERS_HANDLE xHttpReqHeaders,
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
    cfg.buffer_size = (int)HTTP_CLIENT_BUFFER_SIZE;
    cfg.buffer_size_tx = (int)HTTP_CLIENT_BUFFER_SIZE;

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
        ESP_LOGI(TAG, "body: %s", pcBody);
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
                                            esp_http_client_method_t http_method, HTTP_HEADERS_HANDLE xHttpReqHeaders,
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
    cfg.buffer_size = (int)HTTP_CLIENT_BUFFER_SIZE;
    cfg.buffer_size_tx = (int)HTTP_CLIENT_BUFFER_SIZE;

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

put_media_err_t Http_write_chunk(esp_http_client_handle_t http_client, const char *pBuffer, size_t uBytesToSend)
{
    put_media_err_t err = PUT_MEDIA_ERRNO_NONE;
    if (http_client == NULL || pBuffer == NULL || uBytesToSend == 0) {
        err = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if (esp_http_client_chunk_write_begin(http_client, uBytesToSend) != 0) {
        err = PUT_MEDIA_ERROR_NETIO_UNABLE_TO_SEND_CONTENT;
    } else {
        int bytes_sent = 0;
        while (bytes_sent < uBytesToSend) {
            int n = esp_http_client_write(http_client, pBuffer + bytes_sent, uBytesToSend);
            if (n < 0) {
                ESP_LOGE(TAG, "esp_http_client_write error %d", n);
                break;
            }

            bytes_sent += n;
        }

        if (bytes_sent < uBytesToSend) {
            err = PUT_MEDIA_ERROR_NETIO_UNABLE_TO_SEND_CONTENT;
        } else {
            err = esp_http_client_chunk_write_end(http_client, false) == 0 ?
                PUT_MEDIA_ERRNO_NONE : PUT_MEDIA_ERROR_NETIO_UNABLE_TO_SEND_CONTENT;
        }
    }

    return err;
}

put_media_err_t Http_openChunkedRequest(const char *pcHost, const char *pcPort, const char *pcUri,
                                        HTTP_HEADERS_HANDLE xHttpReqHeaders,
                                        unsigned int uRecvTimeoutMs, unsigned int uSendTimeoutMs,
                                        unsigned int *puHttpStatus, esp_http_client_handle_t *ppClient)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    char url[URL_MAX_LEN];
    esp_http_client_config_t cfg = { 0 };
    esp_http_client_handle_t h = NULL;
    esp_err_t err;
    int status;

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
    cfg.buffer_size = (int)HTTP_CLIENT_BUFFER_SIZE;
    cfg.buffer_size_tx = (int)HTTP_CLIENT_BUFFER_SIZE;
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

    err = esp_http_client_fetch_headers(h);
    if (err != ESP_OK) {
        res = PUT_MEDIA_ERROR_RECV_ZERO_SIZED_HTTP_DATA;
        esp_http_client_cleanup(h);
        return res;
    }

    status = esp_http_client_get_status_code(h);
    *puHttpStatus = (unsigned int)status;
    *ppClient = h;
    return PUT_MEDIA_ERRNO_NONE;
}
