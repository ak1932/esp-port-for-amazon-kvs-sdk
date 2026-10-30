/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_CLASS "AwsSignedRequest"

#include <strings.h>

#include "aws_signed_request.h"
#include "aws_signer_v4.h"
#include "esp_crt_bundle.h"

typedef struct {
    PCHAR pBuf;
    UINT32 len;
    UINT32 maxLen;
    // Set once the response outgrows maxLen; esp_http_client ignores a handler's error
    BOOL overflow;
    PAwsSignedHttpRequest pRequest;
} ResponseContext;

/* Headers the request sets itself: a second value under one of these names would be signed but not sent. */
static const PCHAR gReservedHeaders[] = {
    (PCHAR) "host", (PCHAR) "user-agent", (PCHAR) "content-type", (PCHAR) "content-length", (PCHAR) "x-amz-date", (PCHAR) "x-amz-security-token",
    (PCHAR) "authorization",
};

static BOOL prvReservedHeader(PCHAR pName)
{
    for (UINT32 i = 0; i < ARRAY_SIZE(gReservedHeaders); i++) {
        if (strcasecmp(pName, gReservedHeaders[i]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static esp_err_t prvHttpEventHandler(esp_http_client_event_t* evt)
{
    ResponseContext* ctx = (ResponseContext*) evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (ctx->pRequest->serverDateFn != NULL && strcasecmp(evt->header_key, "Date") == 0) {
                ctx->pRequest->serverDateFn(ctx->pRequest->customData, evt->header_value);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (ctx->overflow) {
                break;
            }
            if (ctx->len + evt->data_len > ctx->maxLen) {
                DLOGE("Response longer than %u bytes", ctx->maxLen);
                ctx->overflow = TRUE;
                break;
            }
            MEMCPY(ctx->pBuf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
            ctx->pBuf[ctx->len] = '\0';
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_http_client_method_t prvEspMethod(HTTP_REQUEST_VERB verb)
{
    switch (verb) {
        case HTTP_REQUEST_VERB_GET:
            return HTTP_METHOD_GET;
        case HTTP_REQUEST_VERB_PUT:
            return HTTP_METHOD_PUT;
        default:
            return HTTP_METHOD_POST;
    }
}

STATUS awsSignHttpClient(PRequestInfo pRequestInfo, esp_http_client_handle_t client)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSingleListNode pCurNode = NULL;
    PRequestHeader pRequestHeader;
    UINT64 item;

    CHK(pRequestInfo != NULL && client != NULL, STATUS_NULL_ARG);

    CHK_STATUS(signAwsRequestInfo(pRequestInfo));

    CHK_STATUS(singleListGetHeadNode(pRequestInfo->pRequestHeaders, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(singleListGetNodeData(pCurNode, &item));
        pRequestHeader = (PRequestHeader) HANDLE_TO_POINTER(item);
        CHK(esp_http_client_set_header(client, pRequestHeader->pName, pRequestHeader->pValue) == ESP_OK, STATUS_NOT_ENOUGH_MEMORY);
        CHK_STATUS(singleListGetNextNode(pCurNode, &pCurNode));
    }

CleanUp:
    return retStatus;
}

STATUS awsSignedHttpRequest(PAwsSignedHttpRequest pRequest, PUINT32 pHttpStatus, PCHAR* ppBody, PUINT32 pBodyLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    esp_http_client_handle_t client = NULL;
    esp_http_client_config_t config = {0};
    ResponseContext ctx = {0};
    PRequestInfo pRequestInfo = NULL;
    esp_err_t err = ESP_OK;
    UINT64 timeout;
    PCHAR pUserAgent;

    CHK(pRequest != NULL && pRequest->pUrl != NULL && pHttpStatus != NULL, STATUS_NULL_ARG);
    CHK(pRequest->maxResponseLen > 0 && (pRequest->headerCount == 0 || pRequest->pHeaders != NULL), STATUS_INVALID_ARG);
    for (UINT32 i = 0; i < pRequest->headerCount; i++) {
        CHK(pRequest->pHeaders[i].pName != NULL && pRequest->pHeaders[i].pValue != NULL, STATUS_NULL_ARG);
        CHK_ERR(!prvReservedHeader(pRequest->pHeaders[i].pName), STATUS_INVALID_ARG, "The request sets the %s header itself",
                pRequest->pHeaders[i].pName);
    }
    pRequest->transportErrno = 0;
    pUserAgent = (pRequest->pUserAgent != NULL) ? pRequest->pUserAgent : (PCHAR) DEFAULT_USER_AGENT_NAME;

    ctx.pBuf = (PCHAR) MEMALLOC(pRequest->maxResponseLen + 1);
    CHK(ctx.pBuf != NULL, STATUS_NOT_ENOUGH_MEMORY);
    ctx.pBuf[0] = '\0';
    ctx.maxLen = pRequest->maxResponseLen;
    ctx.pRequest = pRequest;

    config.url = pRequest->pUrl;
    config.method = prvEspMethod(pRequest->verb);
    config.event_handler = prvHttpEventHandler;
    config.user_data = &ctx;
    config.timeout_ms = (int) pRequest->timeoutMs;
    config.buffer_size = 1024;
    config.buffer_size_tx = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_agent = pUserAgent;

    client = esp_http_client_init(&config);
    CHK(client != NULL, STATUS_NOT_ENOUGH_MEMORY);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    if (pRequest->pAwsCredentials != NULL) {
        timeout = (UINT64) pRequest->timeoutMs * HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        CHK_STATUS(createRequestInfo(pRequest->pUrl, pRequest->pBody, pRequest->pRegion, NULL, NULL, NULL, SSL_CERTIFICATE_TYPE_NOT_SPECIFIED,
                                     pUserAgent, timeout, timeout, 0, 0, pRequest->pAwsCredentials, &pRequestInfo));
        pRequestInfo->verb = pRequest->verb;
        for (UINT32 i = 0; i < pRequest->headerCount; i++) {
            CHK_STATUS(setRequestHeader(pRequestInfo, pRequest->pHeaders[i].pName, 0, pRequest->pHeaders[i].pValue, 0));
        }
        awsApplyClockSkew(pRequestInfo, pRequest->clockSkew);
        CHK_STATUS(awsSignHttpClient(pRequestInfo, client));
        freeRequestInfo(&pRequestInfo);
    } else {
        for (UINT32 i = 0; i < pRequest->headerCount; i++) {
            esp_http_client_set_header(client, pRequest->pHeaders[i].pName, pRequest->pHeaders[i].pValue);
        }
    }

    if (pRequest->pBody != NULL) {
        esp_http_client_set_post_field(client, pRequest->pBody, (int) STRLEN(pRequest->pBody));
    }

    for (UINT32 attempt = 0;; attempt++) {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN || attempt >= pRequest->eagainRetries) {
            break;
        }
        THREAD_SLEEP(100 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }
    if (err != ESP_OK) {
        DLOGE("HTTP request to %s failed: %s", pRequest->pUrl, esp_err_to_name(err));
        pRequest->transportErrno = esp_http_client_get_errno(client);
        CHK(FALSE, STATUS_HTTP_NO_RESPONSE);
    }
    CHK(!ctx.overflow, STATUS_HTTP_NO_RESPONSE);

    *pHttpStatus = (UINT32) esp_http_client_get_status_code(client);
    if (ppBody != NULL) {
        *ppBody = ctx.pBuf;
        ctx.pBuf = NULL;
    }
    if (pBodyLen != NULL) {
        *pBodyLen = ctx.len;
    }

CleanUp:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    if (pRequestInfo != NULL) {
        freeRequestInfo(&pRequestInfo);
    }
    SAFE_MEMFREE(ctx.pBuf);
    return retStatus;
}
