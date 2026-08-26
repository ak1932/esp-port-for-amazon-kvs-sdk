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

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Thirdparty headers */
#include "azure_c_shared_utility/doublylinkedlist.h"
#include "azure_c_shared_utility/lock.h"

/* Public headers */
#include "kvs/put_media_errors.h"
#include "kvs/restapi.h"

/* Platform dependent headers */
#include "kvs/port.h"

/* Internal headers */
#include "os/allocator.h"
#include "restful/aws_signer_v4.h"
#include "misc/json_helper.h"
#include "net/http_helper.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

#include "esp_log.h"

#define TAG "PUT_MEDIA_RESTAPI_KVS"

#ifndef SAFE_FREE
#define SAFE_FREE(a) \
    do               \
    {                \
        kvsFree(a);  \
        a = NULL;    \
    } while (0)
#endif /* SAFE_FREE */

#define DEFAULT_RECV_BUFSIZE (1024)

#define PORT_HTTPS "443"

#define URI_SCHEME_HTTPS "https://"
#define URI_SCHEME_HTTPS_LEN (sizeof(URI_SCHEME_HTTPS) - 1)

/* Bounds for draining the PutMedia response after the final chunk is sent. */
#define PUT_MEDIA_DRAIN_TIMEOUT_MS (500)
#define PUT_MEDIA_DRAIN_MAX_READS  (8)

/*-----------------------------------------------------------*/

#define KVS_URI_CREATE_STREAM "/createStream"
#define KVS_URI_DESCRIBE_STREAM "/describeStream"
#define KVS_URI_GET_DATA_ENDPOINT "/getDataEndpoint"
#define KVS_URI_PUT_MEDIA "/putMedia"

/*-----------------------------------------------------------*/

/* KVS caps a stream name at 256 characters and the JSON wrappers below add well under 64. */
#define KVS_HTTP_BODY_MAX_LEN 512

/* Decimal content-length of a body bounded by KVS_HTTP_BODY_MAX_LEN. */
#define KVS_CONTENT_LENGTH_STR_SIZE 12

/* "<seconds>.<milliseconds>": 20 digits of uint64 seconds, a dot, 3 digits, NUL. */
#define PRODUCER_START_TIMESTAMP_STR_SIZE 32

#define DESCRIBE_STREAM_HTTP_BODY_TEMPLATE "{\"StreamName\": \"%s\"}"

#define CREATE_STREAM_HTTP_BODY_TEMPLATE "{\"StreamName\": \"%s\",\"DataRetentionInHours\": %d}"

#define GET_DATA_ENDPOINT_HTTP_BODY_TEMPLATE "{\"StreamName\": \"%s\",\"APIName\":\"PUT_MEDIA\"}"

/*-----------------------------------------------------------*/

typedef struct {
    ePutMediaFragmentAckEventType eventType;
    uint64_t uFragmentTimecode;
    unsigned int uErrorId;

    DLIST_ENTRY xAckEntry;
} FragmentAck_t;

typedef struct PutMedia {
    LOCK_HANDLE xLock;

    esp_http_client_handle_t xHttpClient;
    DLIST_ENTRY xPendingFragmentAcks;
    /* After 100 Continue we must call fetch_headers() once to consume 200 before esp_http_client_read() can return acks */
    int bSecondResponseHeadersFetched;
    /*
     * Set when a chunk write failed after its header was already on the wire.
     * The request body is then malformed: no further chunks may be written and
     * the stream must not be terminated gracefully - only closed.
     */
    bool bStreamCorrupted;
    unsigned int uRecvTimeoutMs;
} PutMedia_t;

#define JSON_KEY_EVENT_TYPE "EventType"
#define JSON_KEY_FRAGMENT_TIMECODE "FragmentTimecode"
#define JSON_KEY_ERROR_ID "ErrorId"

#define EVENT_TYPE_BUFFERING "BUFFERING"
#define EVENT_TYPE_RECEIVED "RECEIVED"
#define EVENT_TYPE_PERSISTED "PERSISTED"
#define EVENT_TYPE_ERROR "ERROR"
#define EVENT_TYPE_IDLE "IDLE"

/*-----------------------------------------------------------*/

static int prvValidateServiceParameter(KvsServiceParameter_t *pServPara)
{
    if (pServPara == NULL || pServPara->pcAccessKey == NULL || pServPara->pcSecretKey == NULL || pServPara->pcRegion == NULL || pServPara->pcService == NULL ||
            pServPara->pcHost == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        return PUT_MEDIA_ERRNO_NONE;
    }
}

static int prvValidateDescribeStreamParameter(KvsDescribeStreamParameter_t *pDescPara)
{
    if (pDescPara == NULL || pDescPara->pcStreamName == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        return PUT_MEDIA_ERRNO_NONE;
    }
}

static int prvValidateCreateStreamParameter(KvsCreateStreamParameter_t *pCreatePara)
{
    if (pCreatePara == NULL || pCreatePara->pcStreamName == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        return PUT_MEDIA_ERRNO_NONE;
    }
}

static int prvValidateGetDataEndpointParameter(KvsGetDataEndpointParameter_t *pGetDataEpPara)
{
    if (pGetDataEpPara == NULL || pGetDataEpPara->pcStreamName == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        return PUT_MEDIA_ERRNO_NONE;
    }
}

static int prvValidatePutMediaParameter(KvsPutMediaParameter_t *pPutMediaPara)
{
    if (pPutMediaPara == NULL || pPutMediaPara->pcStreamName == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        return PUT_MEDIA_ERRNO_NONE;
    }
}

static AwsSigV4Handle prvSign(KvsServiceParameter_t *pServPara, char *pcUri, char *pcQuery, HttpHeaders_t *xHeadersToSign, const char *pcHttpBody)
{
    int res = PUT_MEDIA_ERRNO_NONE;

    AwsSigV4Handle xAwsSigV4Handle = NULL;
    const char *pcVal;
    const char *pcXAmzDate;

    if ((xAwsSigV4Handle = AwsSigV4_Create(HTTP_METHOD_POST_STR, pcUri, pcQuery)) == NULL) {
        res = PUT_MEDIA_ERROR_FAIL_TO_CREATE_SIGV4_HANDLE;
    } else if ((pcVal = HttpHeaders_find(xHeadersToSign, HDR_CONNECTION)) != NULL && AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_CONNECTION, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    // } else if ((pcVal = HttpHeaders_find(xHeadersToSign, HDR_CONTENT_LENGTH)) != NULL && AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_CONTENT_LENGTH, pcVal) != PUT_MEDIA_ERRNO_NONE) {
    //     res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    // } else if ((pcVal = HttpHeaders_find(xHeadersToSign, HDR_CONTENT_TYPE)) != NULL && AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_CONTENT_TYPE, pcVal) != PUT_MEDIA_ERRNO_NONE) {
    //     res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if ((pcVal = HttpHeaders_find(xHeadersToSign, HDR_HOST)) != NULL && AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_HOST, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if (
        (pcVal = HttpHeaders_find(xHeadersToSign, HDR_TRANSFER_ENCODING)) != NULL &&
        AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_TRANSFER_ENCODING, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if ((pcVal = HttpHeaders_find(xHeadersToSign, HDR_USER_AGENT)) != NULL && AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_USER_AGENT, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if (
        (pcXAmzDate = HttpHeaders_find(xHeadersToSign, HDR_X_AMZ_DATE)) != NULL &&
        AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_X_AMZ_DATE, pcXAmzDate) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if (
        (pcVal = HttpHeaders_find(xHeadersToSign, HDR_X_AMZ_SECURITY_TOKEN)) != NULL &&
        AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_X_AMZ_SECURITY_TOKEN, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if (
        (pcVal = HttpHeaders_find(xHeadersToSign, HDR_X_AMZN_FRAG_ACK_REQUIRED)) != NULL &&
        AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_X_AMZN_FRAG_ACK_REQUIRED, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if (
        (pcVal = HttpHeaders_find(xHeadersToSign, HDR_X_AMZN_FRAG_T_TYPE)) != NULL &&
        AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_X_AMZN_FRAG_T_TYPE, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if (
        (pcVal = HttpHeaders_find(xHeadersToSign, HDR_X_AMZN_PRODUCER_START_T)) != NULL &&
        AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_X_AMZN_PRODUCER_START_T, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if (
        (pcVal = HttpHeaders_find(xHeadersToSign, HDR_X_AMZN_STREAM_NAME)) != NULL &&
        AwsSigV4_AddCanonicalHeader(xAwsSigV4Handle, HDR_X_AMZN_STREAM_NAME, pcVal) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_ADD_CANONICAL_HEADER;
    } else if (AwsSigV4_AddCanonicalBody(xAwsSigV4Handle, pcHttpBody, strlen(pcHttpBody)) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERRNO_FAIL;
    } else if ((res = AwsSigV4_Sign(xAwsSigV4Handle, pServPara->pcAccessKey, pServPara->pcSecretKey, pServPara->pcRegion, pServPara->pcService, pcXAmzDate)) != PUT_MEDIA_ERRNO_NONE) {
        /* Propagate the res error */
    } else {
        /* nop */
    }

    if (res != PUT_MEDIA_ERRNO_NONE) {
        AwsSigV4_Terminate(xAwsSigV4Handle);
        xAwsSigV4Handle = NULL;
    }

    return xAwsSigV4Handle;
}

static int prvParseDataEndpoint(const char *pcJsonSrc, size_t uJsonSrcLen, char **ppcEndpoint)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    cJSON *pxRoot = NULL;
    char *pcDataEndpoint = NULL;
    size_t uEndpointLen = 0;

    if (pcJsonSrc == NULL || uJsonSrcLen == 0 || ppcEndpoint == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if (
        (pxRoot = cJSON_ParseWithLength(pcJsonSrc, uJsonSrcLen)) == NULL ||
        (pcDataEndpoint = json_dotget_string(pxRoot, "DataEndpoint")) == NULL) {
        res = PUT_MEDIA_ERROR_FAIL_TO_PARSE_DATA_ENDPOINT;
        ESP_LOGE(TAG, "Failed to parse data endpoint");
    } else {
        /* The service returns "https://<host>"; strip the scheme and hand the host to the caller. */
        uEndpointLen = strlen(pcDataEndpoint);
        if (uEndpointLen <= URI_SCHEME_HTTPS_LEN || strncmp(pcDataEndpoint, URI_SCHEME_HTTPS, URI_SCHEME_HTTPS_LEN) != 0) {
            res = PUT_MEDIA_ERROR_FAIL_TO_PARSE_DATA_ENDPOINT;
            ESP_LOGE(TAG, "Unexpected data endpoint format:%s", pcDataEndpoint);
        } else {
            uEndpointLen -= URI_SCHEME_HTTPS_LEN;
            *ppcEndpoint = (char *)kvsMalloc(uEndpointLen + 1);
            if (*ppcEndpoint == NULL) {
                res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
                ESP_LOGE(TAG, "OOM: data endpoint");
            } else {
                memcpy(*ppcEndpoint, pcDataEndpoint + URI_SCHEME_HTTPS_LEN, uEndpointLen);
                (*ppcEndpoint)[uEndpointLen] = '\0';
            }
        }
        kvsFree(pcDataEndpoint);
    }

    cJSON_Delete(pxRoot);

    return res;
}

static char *prvGetTimecodeValue(FragmentTimecodeType_t xTimecodeType)
{
    if (xTimecodeType == TIMECODE_TYPE_ABSOLUTE) {
        return "ABSOLUTE";
    } else if (xTimecodeType == TIMECODE_TYPE_RELATIVE) {
        return "RELATIVE";
    } else {
        ESP_LOGE(TAG, "Invalid timecode type:%d", xTimecodeType);
        return "";
    }
}

static int prvGetEpochTimestampInStr(uint64_t uProducerStartTimestampMs, char *pcBuf, size_t uBufSize)
{
    uint64_t uProducerStartTimestamp = 0;

    if (pcBuf == NULL || uBufSize == 0) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

    uProducerStartTimestamp = (uProducerStartTimestampMs == 0) ? getEpochTimestampInMs() : uProducerStartTimestampMs;

    if (snprintf(pcBuf, uBufSize, "%" PRIu64 ".%03d", uProducerStartTimestamp / 1000, (int)(uProducerStartTimestamp % 1000)) >= (int)uBufSize) {
        return PUT_MEDIA_ERROR_STRING_ERROR;
    }

    return PUT_MEDIA_ERRNO_NONE;
}

/*
 * Render a request body from a template together with its content-length; the
 * three JSON control-plane calls below need both, and always as a matched pair.
 */
static int prvFormatHttpBody(char *pcBody, size_t uBodySize, char *pcContentLength, size_t uContentLengthSize, const char *pcFormat, ...)
{
    va_list xArgs;
    int iLen = 0;

    va_start(xArgs, pcFormat);
    iLen = vsnprintf(pcBody, uBodySize, pcFormat, xArgs);
    va_end(xArgs);

    if (iLen < 0 || (size_t)iLen >= uBodySize) {
        return PUT_MEDIA_ERROR_STRING_ERROR;
    }
    if (snprintf(pcContentLength, uContentLengthSize, "%d", iLen) >= (int)uContentLengthSize) {
        return PUT_MEDIA_ERROR_STRING_ERROR;
    }

    return PUT_MEDIA_ERRNO_NONE;
}

static int prvParseFragmentAckLength(char *pcSrc, size_t uLen, size_t *puMsgLen, size_t *puBytesRead)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    size_t uMsgLen = 0;
    size_t uBytesRead = 0;
    size_t i = 0;
    char c = 0;

    if (pcSrc == NULL || uLen == 0 || puMsgLen == NULL || puBytesRead == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        for (i = 0; i < uLen - 1; i++) {
            c = toupper(pcSrc[i]);
            if (isxdigit(c)) {
                if (c >= '0' && c <= '9') {
                    uMsgLen = uMsgLen * 16 + (c - '0');
                } else {
                    uMsgLen = uMsgLen * 16 + (c - 'A') + 10;
                }
            } else if (c == '\r') {
                if (pcSrc[i + 1] == '\n') {
                    uBytesRead = i + 2;
                    break;
                }
            } else {
                res = PUT_MEDIA_ERROR_FAIL_TO_PARSE_FRAGMENT_ACK_LENGTH;
            }
        }
    }

    if (res == PUT_MEDIA_ERRNO_NONE) {
        if (uBytesRead < 3 || (uBytesRead + uMsgLen + 2) > uLen || pcSrc[uBytesRead + uMsgLen] != '\r' || pcSrc[uBytesRead + uMsgLen + 1] != '\n') {
            res = PUT_MEDIA_ERROR_FAIL_TO_PARSE_FRAGMENT_ACK_LENGTH;
        } else {
            *puMsgLen = uMsgLen;
            *puBytesRead = uBytesRead;
        }
    }

    return res;
}

static ePutMediaFragmentAckEventType prvGetEventType(char *pcEventType)
{
    ePutMediaFragmentAckEventType ev = eUnknown;

    if (pcEventType != NULL) {
        if (strcmp(pcEventType, EVENT_TYPE_BUFFERING) == 0) {
            ev = eBuffering;
        } else if (strcmp(pcEventType, EVENT_TYPE_RECEIVED) == 0) {
            ev = eReceived;
        } else if (strcmp(pcEventType, EVENT_TYPE_PERSISTED) == 0) {
            ev = ePersisted;
        } else if (strcmp(pcEventType, EVENT_TYPE_ERROR) == 0) {
            ev = eError;
        } else if (strcmp(pcEventType, EVENT_TYPE_IDLE) == 0) {
            ev = eIdle;
        }
    }

    return ev;
}

static int parseFragmentMsg(const char *pcFragmentMsg, size_t uFragmentMsgLen, FragmentAck_t *pxFragmentAck)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    cJSON *pxRoot = NULL;
    char *pcEventType = NULL;

    if (pcFragmentMsg == NULL || uFragmentMsgLen == 0 || pxFragmentAck == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if ((pxRoot = cJSON_ParseWithLength(pcFragmentMsg, uFragmentMsgLen)) == NULL) {
        res = PUT_MEDIA_ERROR_FAIL_TO_PARSE_FRAGMENT_ACK_MSG;
        ESP_LOGI(TAG, "Failed to parse fragment msg:%.*s", (int)uFragmentMsgLen, pcFragmentMsg);
    } else if ((pcEventType = json_dotget_string(pxRoot, JSON_KEY_EVENT_TYPE)) == NULL) {
        res = PUT_MEDIA_ERROR_UNKNOWN_FRAGMENT_ACK_TYPE;
        ESP_LOGI(TAG, "Unknown fragment ack:%.*s", (int)uFragmentMsgLen, pcFragmentMsg);
    } else {
        pxFragmentAck->eventType = prvGetEventType(pcEventType);
        kvsFree(pcEventType);

        if (pxFragmentAck->eventType == eBuffering || pxFragmentAck->eventType == eReceived || pxFragmentAck->eventType == ePersisted ||
                pxFragmentAck->eventType == eError) {
            pxFragmentAck->uFragmentTimecode = json_dotget_uint64(pxRoot, JSON_KEY_FRAGMENT_TIMECODE, 10);
            if (pxFragmentAck->eventType == eError) {
                pxFragmentAck->uErrorId = (unsigned int)json_dotget_uint64(pxRoot, JSON_KEY_ERROR_ID, 10);
            }
        }
    }

    cJSON_Delete(pxRoot);

    return res;
}

static int prvParseFragmentAck(char *pcSrc, size_t uLen, FragmentAck_t *pxFragAck, size_t *puFragAckLen)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    size_t uMsgLen = 0;
    size_t uBytesRead = 0;

    if (pcSrc == NULL || uLen == 0 || pxFragAck == NULL || puFragAckLen == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if ((res = prvParseFragmentAckLength(pcSrc, uLen, &uMsgLen, &uBytesRead)) != PUT_MEDIA_ERRNO_NONE) {
        /*
         * esp_http_client_read() returns chunk-decoded body — there is
         * no "hex\r\n" prefix.  Fall back to treating each line as a
         * bare JSON object terminated by \n (or \r\n).
         */
        res = PUT_MEDIA_ERRNO_NONE;
        size_t uLineLen = 0;
        for (size_t i = 0; i < uLen; i++) {
            if (pcSrc[i] == '\n') {
                uLineLen = i + 1;
                break;
            }
        }
        if (uLineLen == 0) {
            uLineLen = uLen;
        }
        if (uLineLen > 0 && pcSrc[0] == '{') {
            res = parseFragmentMsg(pcSrc, uLineLen, pxFragAck);
            if (res == PUT_MEDIA_ERRNO_NONE) {
                *puFragAckLen = uLineLen;
            }
        } else {
            res = PUT_MEDIA_ERROR_FAIL_TO_PARSE_FRAGMENT_ACK_MSG;
            ESP_LOGI(TAG, "Unknown fragment ack:%.*s", (int)uLen, pcSrc);
        }
    } else {
        if (parseFragmentMsg(pcSrc + uBytesRead, uMsgLen, pxFragAck) != PUT_MEDIA_ERRNO_NONE) {
            res = PUT_MEDIA_ERROR_FAIL_TO_PARSE_FRAGMENT_ACK_MSG;
            ESP_LOGI(TAG, "Failed to parse fragment ack");
        } else {
            *puFragAckLen = uBytesRead + uMsgLen + 2;
        }
    }

    return res;
}

static void prvLogFragmentAck(FragmentAck_t *pFragmentAck)
{
    if (pFragmentAck != NULL) {
        if (pFragmentAck->eventType == eBuffering) {
            ESP_LOGV(TAG, "Fragment buffering, timecode:%" PRIu64 "", pFragmentAck->uFragmentTimecode);
        } else if (pFragmentAck->eventType == eReceived) {
            ESP_LOGV(TAG, "Fragment received, timecode:%" PRIu64 "", pFragmentAck->uFragmentTimecode);
        } else if (pFragmentAck->eventType == ePersisted) {
            ESP_LOGV(TAG, "Fragment persisted, timecode:%" PRIu64 "", pFragmentAck->uFragmentTimecode);
        } else if (pFragmentAck->eventType == eError) {
            ESP_LOGV(TAG, "PutMedia session error id:%d", pFragmentAck->uErrorId);
        } else if (pFragmentAck->eventType == eIdle) {
            ESP_LOGV(TAG, "PutMedia session Idle");
        } else {
            ESP_LOGW(TAG, "Unknown Fragment Ack: %d", pFragmentAck->eventType);
        }
    }
}

static void prvLogPendingFragmentAcks(PutMedia_t *pPutMedia)
{
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;
    FragmentAck_t *pFragmentAck = NULL;

    if (pPutMedia != NULL && Lock(pPutMedia->xLock) == LOCK_OK) {
        pxListHead = &(pPutMedia->xPendingFragmentAcks);
        pxListItem = pxListHead->Flink;
        while (pxListItem != pxListHead) {
            pFragmentAck = containingRecord(pxListItem, FragmentAck_t, xAckEntry);
            prvLogFragmentAck(pFragmentAck);

            pxListItem = pxListItem->Flink;
        }

        Unlock(pPutMedia->xLock);
    }
}

static int prvPushFragmentAck(PutMedia_t *pPutMedia, FragmentAck_t *pFragmentAckSrc)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    FragmentAck_t *pFragmentAck = NULL;

    if (pPutMedia == NULL || pFragmentAckSrc == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if ((pFragmentAck = (FragmentAck_t *)kvsMalloc(sizeof(FragmentAck_t))) == NULL) {
        res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
    } else {
        memcpy(pFragmentAck, pFragmentAckSrc, sizeof(FragmentAck_t));
        DList_InitializeListHead(&(pFragmentAck->xAckEntry));

        if (Lock(pPutMedia->xLock) != LOCK_OK) {
            res = PUT_MEDIA_ERROR_LOCK_ERROR;
        } else {
            DList_InsertTailList(&(pPutMedia->xPendingFragmentAcks), &(pFragmentAck->xAckEntry));

            Unlock(pPutMedia->xLock);
        }
    }

    if (res != PUT_MEDIA_ERRNO_NONE) {
        if (pFragmentAck != NULL) {
            kvsFree(pFragmentAck);
        }
    }

    return res;
}

static PutMedia_t *prvCreateDefaultPutMediaHandle()
{
    int res = PUT_MEDIA_ERRNO_NONE;
    PutMedia_t *pPutMedia = NULL;

    if ((pPutMedia = (PutMedia_t *)kvsMalloc(sizeof(PutMedia_t))) == NULL) {
        res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
        ESP_LOGE(TAG, "OOM: pPutMedia");
    } else {
        memset(pPutMedia, 0, sizeof(PutMedia_t));

        if ((pPutMedia->xLock = Lock_Init()) == NULL) {
            res = PUT_MEDIA_ERROR_LOCK_ERROR;
            ESP_LOGE(TAG, "Failed to initialize lock");
        } else {
            DList_InitializeListHead(&(pPutMedia->xPendingFragmentAcks));
            pPutMedia->bSecondResponseHeadersFetched = 0;
        }
    }

    if (res != PUT_MEDIA_ERRNO_NONE) {
        if (pPutMedia != NULL) {
            if (pPutMedia->xLock != NULL) {
                Lock_Deinit(pPutMedia->xLock);
            }
            kvsFree(pPutMedia);
            pPutMedia = NULL;
        }
    }

    return pPutMedia;
}

static FragmentAck_t *prvReadFragmentAck(PutMedia_t *pPutMedia)
{
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;
    FragmentAck_t *pFragmentAck = NULL;

    if (Lock(pPutMedia->xLock) == LOCK_OK) {
        if (!DList_IsListEmpty(&(pPutMedia->xPendingFragmentAcks))) {
            pxListHead = &(pPutMedia->xPendingFragmentAcks);
            pxListItem = DList_RemoveHeadList(pxListHead);
            pFragmentAck = containingRecord(pxListItem, FragmentAck_t, xAckEntry);
        }
        Unlock(pPutMedia->xLock);
    }

    return pFragmentAck;
}

static void prvFlushFragmentAck(PutMedia_t *pPutMedia)
{
    FragmentAck_t *pFragmentAck = NULL;
    while ((pFragmentAck = prvReadFragmentAck(pPutMedia)) != NULL) {
        kvsFree(pFragmentAck);
    }
}

int Kvs_describeStream(KvsServiceParameter_t *pServPara, KvsDescribeStreamParameter_t *pDescPara, unsigned int *puHttpStatusCode)
{
    int res = PUT_MEDIA_ERRNO_NONE;

    char pcHttpBody[KVS_HTTP_BODY_MAX_LEN] = {0};
    char pcContentLength[KVS_CONTENT_LENGTH_STR_SIZE] = {0};
    char pcXAmzDate[DATE_TIME_ISO_8601_FORMAT_STRING_SIZE] = {0};

    AwsSigV4Handle xAwsSigV4Handle = NULL;

    unsigned int uHttpStatusCode = 0;
    HttpHeaders_t *xHttpReqHeaders = NULL;
    char *pRspBody = NULL;
    size_t uRspBodyLen = 0;

    if (puHttpStatusCode != NULL) {
        *puHttpStatusCode = 0; /* Set to zero to avoid misuse from the previous value. */
    }

    if ((res = prvValidateServiceParameter(pServPara)) != PUT_MEDIA_ERRNO_NONE ||
            (res = prvValidateDescribeStreamParameter(pDescPara)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Invalid argument");
        /* Propagate the res error */
    } else {
        ESP_LOGI(TAG, "Making describe request to: %s", pServPara->pcHost);
        ESP_LOGI(TAG, "Stream name: %s", pDescPara->pcStreamName);
        if ((res = getTimeInIso8601(pcXAmzDate, sizeof(pcXAmzDate))) != PUT_MEDIA_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed to get time");
            /* Propagate the res error */
        } else if (
            prvFormatHttpBody(
                pcHttpBody, sizeof(pcHttpBody), pcContentLength, sizeof(pcContentLength), DESCRIBE_STREAM_HTTP_BODY_TEMPLATE, pDescPara->pcStreamName) !=
                PUT_MEDIA_ERRNO_NONE) {
            res = PUT_MEDIA_ERROR_UNABLE_TO_ALLOCATE_HTTP_BODY;
            ESP_LOGE(TAG, "Failed to allocate HTTP body");
        } else if (
            (xHttpReqHeaders = HttpHeaders_create()) == NULL ||
            HttpHeaders_add(xHttpReqHeaders, HDR_HOST, pServPara->pcHost) != PUT_MEDIA_ERRNO_NONE ||
            HttpHeaders_add(xHttpReqHeaders, HDR_ACCEPT, VAL_ACCEPT_ANY) != PUT_MEDIA_ERRNO_NONE ||
            HttpHeaders_add(xHttpReqHeaders, HDR_CONTENT_LENGTH, pcContentLength) != PUT_MEDIA_ERRNO_NONE ||
            HttpHeaders_add(xHttpReqHeaders, HDR_CONTENT_TYPE, VAL_CONTENT_TYPE_APPLICATION_jSON) != PUT_MEDIA_ERRNO_NONE ||
            HttpHeaders_add(xHttpReqHeaders, HDR_USER_AGENT, VAL_USER_AGENT) != PUT_MEDIA_ERRNO_NONE ||
            HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZ_DATE, pcXAmzDate) != PUT_MEDIA_ERRNO_NONE ||
            (pServPara->pcToken != NULL && (HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZ_SECURITY_TOKEN, pServPara->pcToken) != PUT_MEDIA_ERRNO_NONE))) {
            res = PUT_MEDIA_ERROR_UNABLE_TO_GENERATE_HTTP_HEADER;
            ESP_LOGE(TAG, "Failed to generate HTTP headers");
        } else if (
            (xAwsSigV4Handle = prvSign(pServPara, KVS_URI_DESCRIBE_STREAM, URI_QUERY_EMPTY, xHttpReqHeaders, pcHttpBody)) == NULL ||
            HttpHeaders_add(xHttpReqHeaders, HDR_AUTHORIZATION, AwsSigV4_GetAuthorization(xAwsSigV4Handle)) != PUT_MEDIA_ERRNO_NONE) {
            res = PUT_MEDIA_ERROR_FAIL_TO_SIGN_HTTP_REQ;
            ESP_LOGE(TAG, "Failed to sign");
        } else if ((res = Http_performRequest(pServPara->pcHost, PORT_HTTPS, KVS_URI_DESCRIBE_STREAM, HTTP_METHOD_POST,
                    xHttpReqHeaders, pcHttpBody, pServPara->uRecvTimeoutMs, pServPara->uSendTimeoutMs,
                    &uHttpStatusCode, &pRspBody, &uRspBodyLen)) != PUT_MEDIA_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed http request to %s", pServPara->pcHost);
            /* Propagate the res error */
        } else {
            if (puHttpStatusCode != NULL) {
                *puHttpStatusCode = uHttpStatusCode;
            }

            if (uHttpStatusCode != 200) {
                ESP_LOGI(TAG, "Describe Stream failed, HTTP status code: %u", uHttpStatusCode);
                ESP_LOGI(TAG, "HTTP response message:%.*s", (int)uRspBodyLen, pRspBody);
            }
        }

        SAFE_FREE(pRspBody);
        HttpHeaders_free(xHttpReqHeaders);
        AwsSigV4_Terminate(xAwsSigV4Handle);
    }
    return res;
}

int Kvs_createStream(KvsServiceParameter_t *pServPara, KvsCreateStreamParameter_t *pCreatePara, unsigned int *puHttpStatusCode)
{
    int res = PUT_MEDIA_ERRNO_NONE;

    char pcHttpBody[KVS_HTTP_BODY_MAX_LEN] = {0};
    char pcContentLength[KVS_CONTENT_LENGTH_STR_SIZE] = {0};
    char pcXAmzDate[DATE_TIME_ISO_8601_FORMAT_STRING_SIZE] = {0};

    AwsSigV4Handle xAwsSigV4Handle = NULL;

    unsigned int uHttpStatusCode = 0;
    HttpHeaders_t *xHttpReqHeaders = NULL;
    char *pRspBody = NULL;
    size_t uRspBodyLen = 0;

    if (puHttpStatusCode != NULL) {
        *puHttpStatusCode = 0; /* Set to zero to avoid misuse from previous value. */
    }

    if ((res = prvValidateServiceParameter(pServPara)) != PUT_MEDIA_ERRNO_NONE ||
            (res = prvValidateCreateStreamParameter(pCreatePara)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Invalid argument");
        /* Propagate the res error */
    } else if ((res = getTimeInIso8601(pcXAmzDate, sizeof(pcXAmzDate))) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to get time");
        /* Propagate the res error */
    } else if (
        prvFormatHttpBody(
            pcHttpBody, sizeof(pcHttpBody), pcContentLength, sizeof(pcContentLength), CREATE_STREAM_HTTP_BODY_TEMPLATE, pCreatePara->pcStreamName,
            pCreatePara->uDataRetentionInHours) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_UNABLE_TO_ALLOCATE_HTTP_BODY;
        ESP_LOGE(TAG, "Failed to allocate HTTP body");
    } else if (
        (xHttpReqHeaders = HttpHeaders_create()) == NULL ||
        HttpHeaders_add(xHttpReqHeaders, HDR_HOST, pServPara->pcHost) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_ACCEPT, VAL_ACCEPT_ANY) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_CONTENT_LENGTH, pcContentLength) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_CONTENT_TYPE, VAL_CONTENT_TYPE_APPLICATION_jSON) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_USER_AGENT, VAL_USER_AGENT) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZ_DATE, pcXAmzDate) != PUT_MEDIA_ERRNO_NONE ||
        (pServPara->pcToken != NULL && (HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZ_SECURITY_TOKEN, pServPara->pcToken) != PUT_MEDIA_ERRNO_NONE))) {
        res = PUT_MEDIA_ERROR_UNABLE_TO_GENERATE_HTTP_HEADER;
        ESP_LOGE(TAG, "Failed to generate HTTP headers");
    } else if (
        (xAwsSigV4Handle = prvSign(pServPara, KVS_URI_CREATE_STREAM, URI_QUERY_EMPTY, xHttpReqHeaders, pcHttpBody)) == NULL ||
        HttpHeaders_add(xHttpReqHeaders, HDR_AUTHORIZATION, AwsSigV4_GetAuthorization(xAwsSigV4Handle)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to sign");
        res = PUT_MEDIA_ERROR_FAIL_TO_SIGN_HTTP_REQ;
    } else if ((res = Http_performRequest(pServPara->pcHost, PORT_HTTPS, KVS_URI_CREATE_STREAM, HTTP_METHOD_POST,
                xHttpReqHeaders, pcHttpBody, pServPara->uRecvTimeoutMs, pServPara->uSendTimeoutMs,
                &uHttpStatusCode, &pRspBody, &uRspBodyLen)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed http request to %s", pServPara->pcHost);
        /* Propagate the res error */
    } else {
        if (puHttpStatusCode != NULL) {
            *puHttpStatusCode = uHttpStatusCode;
        }

        if (uHttpStatusCode != 200) {
            ESP_LOGI(TAG, "Create Stream failed, HTTP status code: %u", uHttpStatusCode);
            ESP_LOGI(TAG, "HTTP response message:%.*s", (int)uRspBodyLen, pRspBody);
        }
    }

    SAFE_FREE(pRspBody);
    HttpHeaders_free(xHttpReqHeaders);
    AwsSigV4_Terminate(xAwsSigV4Handle);

    return res;
}

int Kvs_getDataEndpoint(KvsServiceParameter_t *pServPara, KvsGetDataEndpointParameter_t *pGetDataEpPara, unsigned int *puHttpStatusCode, char **ppcDataEndpoint)
{
    int res = PUT_MEDIA_ERRNO_NONE;

    char pcHttpBody[KVS_HTTP_BODY_MAX_LEN] = {0};
    char pcContentLength[KVS_CONTENT_LENGTH_STR_SIZE] = {0};
    char pcXAmzDate[DATE_TIME_ISO_8601_FORMAT_STRING_SIZE] = {0};

    AwsSigV4Handle xAwsSigV4Handle = NULL;

    unsigned int uHttpStatusCode = 0;
    HttpHeaders_t *xHttpReqHeaders = NULL;
    char *pRspBody = NULL;
    size_t uRspBodyLen = 0;

    if (puHttpStatusCode != NULL) {
        *puHttpStatusCode = 0; /* Set to zero to avoid misuse from previous value. */
    }

    if ((res = prvValidateServiceParameter(pServPara)) != PUT_MEDIA_ERRNO_NONE ||
            (res = prvValidateGetDataEndpointParameter(pGetDataEpPara)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Invalid argument");
        /* Propagate the res error */
    } else if ((res = getTimeInIso8601(pcXAmzDate, sizeof(pcXAmzDate))) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to get time");
        /* Propagate the res error */
    } else if (
        prvFormatHttpBody(
            pcHttpBody, sizeof(pcHttpBody), pcContentLength, sizeof(pcContentLength), GET_DATA_ENDPOINT_HTTP_BODY_TEMPLATE, pGetDataEpPara->pcStreamName) !=
            PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_UNABLE_TO_ALLOCATE_HTTP_BODY;
        ESP_LOGE(TAG, "Failed to allocate HTTP body");
    } else if (
        (xHttpReqHeaders = HttpHeaders_create()) == NULL ||
        HttpHeaders_add(xHttpReqHeaders, HDR_HOST, pServPara->pcHost) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_ACCEPT, VAL_ACCEPT_ANY) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_CONTENT_LENGTH, pcContentLength) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_CONTENT_TYPE, VAL_CONTENT_TYPE_APPLICATION_jSON) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_USER_AGENT, VAL_USER_AGENT) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZ_DATE, pcXAmzDate) != PUT_MEDIA_ERRNO_NONE ||
        (pServPara->pcToken != NULL && (HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZ_SECURITY_TOKEN, pServPara->pcToken) != PUT_MEDIA_ERRNO_NONE))) {
        res = PUT_MEDIA_ERROR_UNABLE_TO_GENERATE_HTTP_HEADER;
        ESP_LOGE(TAG, "Failed to generate HTTP headers");
    } else if (
        (xAwsSigV4Handle = prvSign(pServPara, KVS_URI_GET_DATA_ENDPOINT, URI_QUERY_EMPTY, xHttpReqHeaders, pcHttpBody)) == NULL ||
        HttpHeaders_add(xHttpReqHeaders, HDR_AUTHORIZATION, AwsSigV4_GetAuthorization(xAwsSigV4Handle)) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_SIGN_HTTP_REQ;
        ESP_LOGE(TAG, "Failed to sign");
    } else if ((res = Http_performRequest(pServPara->pcHost, PORT_HTTPS, KVS_URI_GET_DATA_ENDPOINT, HTTP_METHOD_POST,
                xHttpReqHeaders, pcHttpBody, pServPara->uRecvTimeoutMs, pServPara->uSendTimeoutMs,
                &uHttpStatusCode, &pRspBody, &uRspBodyLen)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed http request to %s", pServPara->pcHost);
        /* Propagate the res error */
    } else {
        if (puHttpStatusCode != NULL) {
            *puHttpStatusCode = uHttpStatusCode;
        }

        if (uHttpStatusCode != 200) {
            ESP_LOGI(TAG, "Get Data Endpoint failed, HTTP status code: %u", uHttpStatusCode);
            ESP_LOGI(TAG, "HTTP response message:%.*s", (int)uRspBodyLen, pRspBody);
        } else {
            if ((res = prvParseDataEndpoint(pRspBody, uRspBodyLen, ppcDataEndpoint)) != PUT_MEDIA_ERRNO_NONE) {
                ESP_LOGE(TAG, "Failed to parse data endpoint");
                /* Propagate the res error */
            }
        }
    }

    SAFE_FREE(pRspBody);
    HttpHeaders_free(xHttpReqHeaders);
    AwsSigV4_Terminate(xAwsSigV4Handle);

    return res;
}

int Kvs_putMediaStart(KvsServiceParameter_t *pServPara, KvsPutMediaParameter_t *pPutMediaPara, unsigned int *puHttpStatusCode, PutMediaHandle *pPutMediaHandle)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    PutMedia_t *pPutMedia = NULL;

    char pcXAmzDate[DATE_TIME_ISO_8601_FORMAT_STRING_SIZE] = {0};
    char pcProducerStartTimestamp[PRODUCER_START_TIMESTAMP_STR_SIZE] = {0};

    AwsSigV4Handle xAwsSigV4Handle = NULL;

    unsigned int uHttpStatusCode = 0;
    HttpHeaders_t *xHttpReqHeaders = NULL;

    esp_http_client_handle_t xHttpClient = NULL;
    bool bKeepClient = false;

    if (puHttpStatusCode != NULL) {
        *puHttpStatusCode = 0; /* Set to zero to avoid misuse from previous value. */
    }

    if ((res = prvValidateServiceParameter(pServPara)) != PUT_MEDIA_ERRNO_NONE ||
            (res = prvValidatePutMediaParameter(pPutMediaPara)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Invalid argument");
        /* Propagate the res error */
    } else if (pPutMediaHandle == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if ((res = getTimeInIso8601(pcXAmzDate, sizeof(pcXAmzDate))) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to get time");
        /* Propagate the res error */
    } else if ((res = prvGetEpochTimestampInStr(pPutMediaPara->uProducerStartTimestampMs, pcProducerStartTimestamp, sizeof(pcProducerStartTimestamp))) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to get epoch time");
        /* Propagate the res error */
    } else if (
        (xHttpReqHeaders = HttpHeaders_create()) == NULL ||
        HttpHeaders_add(xHttpReqHeaders, HDR_HOST, pServPara->pcPutMediaEndpoint) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_ACCEPT, VAL_ACCEPT_ANY) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_CONNECTION, VAL_KEEP_ALIVE) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_CONTENT_TYPE, VAL_CONTENT_TYPE_APPLICATION_jSON) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_TRANSFER_ENCODING, VAL_TRANSFER_ENCODING_CHUNKED) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_USER_AGENT, VAL_USER_AGENT) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZ_DATE, pcXAmzDate) != PUT_MEDIA_ERRNO_NONE ||
        (pServPara->pcToken != NULL && (HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZ_SECURITY_TOKEN, pServPara->pcToken) != PUT_MEDIA_ERRNO_NONE)) ||
        HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZN_FRAG_ACK_REQUIRED, VAL_FRAGMENT_ACK_REQUIRED_TRUE) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZN_FRAG_T_TYPE, prvGetTimecodeValue(pPutMediaPara->xTimecodeType)) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZN_PRODUCER_START_T, pcProducerStartTimestamp) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZN_STREAM_NAME, pPutMediaPara->pcStreamName) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, "expect", "100-continue") != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_UNABLE_TO_GENERATE_HTTP_HEADER;
        ESP_LOGE(TAG, "Failed to generate HTTP headers");
    } else if (
        (xAwsSigV4Handle = prvSign(pServPara, KVS_URI_PUT_MEDIA, URI_QUERY_EMPTY, xHttpReqHeaders, HTTP_BODY_EMPTY)) == NULL ||
        HttpHeaders_add(xHttpReqHeaders, HDR_AUTHORIZATION, AwsSigV4_GetAuthorization(xAwsSigV4Handle)) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_SIGN_HTTP_REQ;
        ESP_LOGE(TAG, "Failed to sign");
    } else if ((res = Http_openChunkedRequest(pServPara->pcPutMediaEndpoint, PORT_HTTPS, KVS_URI_PUT_MEDIA,
                xHttpReqHeaders, pServPara->uRecvTimeoutMs, pServPara->uSendTimeoutMs,
                &uHttpStatusCode, &xHttpClient)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to open chunked request to %s", pServPara->pcPutMediaEndpoint);
        /* Propagate the res error */
    } else {
        if (puHttpStatusCode != NULL) {
            *puHttpStatusCode = uHttpStatusCode;
        }

        if (uHttpStatusCode != 100 && uHttpStatusCode != 200) {
            ESP_LOGE(TAG, "Put Media failed, HTTP status code: %u", uHttpStatusCode);
            res = PUT_MEDIA_GENERATE_RESTFUL_ERROR(uHttpStatusCode);
            esp_http_client_cleanup(xHttpClient);
            xHttpClient = NULL;
        } else {
            if ((pPutMedia = prvCreateDefaultPutMediaHandle()) == NULL) {
                res = PUT_MEDIA_ERROR_FAIL_TO_CREATE_PUT_MEDIA_HANDLE;
                ESP_LOGE(TAG, "Failed to create pPutMedia");
                esp_http_client_cleanup(xHttpClient);
            } else {
                pPutMedia->xHttpClient = xHttpClient;
                pPutMedia->uRecvTimeoutMs = pPutMediaPara->uRecvTimeoutMs;
                *pPutMediaHandle = pPutMedia;
                Kvs_putMediaUpdateRecvTimeout(pPutMedia, pPutMediaPara->uRecvTimeoutMs);
                bKeepClient = true;
            }
        }
    }

    if (!bKeepClient && xHttpClient != NULL) {
        esp_http_client_cleanup(xHttpClient);
    }
    HttpHeaders_free(xHttpReqHeaders);
    AwsSigV4_Terminate(xAwsSigV4Handle);

    return res;
}

/*
 * Write one chunk, refusing to touch a stream that a previous failure already
 * left mid-chunk and latching that state when a new failure happens. Once this
 * returns an error the caller must abandon the session: the request body cannot
 * be repaired, and writing the next chunk header into it would corrupt whatever
 * the service has parsed so far.
 */
static int prvWriteChunkChecked(PutMedia_t *pPutMedia, const uint8_t *pBuf, size_t uLen)
{
    int res = PUT_MEDIA_ERRNO_NONE;

    if (pPutMedia->bStreamCorrupted) {
        res = PUT_MEDIA_ERROR_INVALID_STATE;
    } else if ((res = Http_write_chunk(pPutMedia->xHttpClient, (const char *)pBuf, uLen)) != PUT_MEDIA_ERRNO_NONE) {
        pPutMedia->bStreamCorrupted = true;
    }

    return res;
}

int Kvs_putMediaUpdate(PutMediaHandle xPutMediaHandle, uint8_t *pMkvHeader, size_t uMkvHeaderLen, uint8_t *pData, size_t uDataLen)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    PutMedia_t *pPutMedia = xPutMediaHandle;

    if (pData == NULL) {
        uDataLen = 0;
    }

    if (pPutMedia == NULL || pMkvHeader == NULL || uMkvHeaderLen == 0) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if (uDataLen == 0) {
        if ((res = prvWriteChunkChecked(pPutMedia, pMkvHeader, uMkvHeaderLen)) != PUT_MEDIA_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed to send MKV header chunk");
        }
    } else if (uMkvHeaderLen + uDataLen < uMkvHeaderLen) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Block length overflows");
    } else {
        /*
         * The MKV header and the frame it describes go out as one chunk.
         * TODO: verify if this actually improves
         */
        size_t uBlockLen = uMkvHeaderLen + uDataLen;
        uint8_t *pBlock = (uint8_t *)heap_caps_malloc_prefer(uBlockLen, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);

        if (pBlock == NULL) {
            res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
            ESP_LOGE(TAG, "No memory to join a %zu byte block", uBlockLen);
        } else {
            memcpy(pBlock, pMkvHeader, uMkvHeaderLen);
            memcpy(pBlock + uMkvHeaderLen, pData, uDataLen);

            res = prvWriteChunkChecked(pPutMedia, pBlock, uBlockLen);
            free(pBlock);

            if (res != PUT_MEDIA_ERRNO_NONE) {
                ESP_LOGE(TAG, "Failed to send data frame");
            }
        }
    }

    return res;
}

int Kvs_putMediaUpdateRaw(PutMediaHandle xPutMediaHandle, uint8_t *pBuf, size_t uLen)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    PutMedia_t *pPutMedia = xPutMediaHandle;

    if (pPutMedia == NULL || pBuf == NULL || uLen == 0) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else {
        if ((res = prvWriteChunkChecked(pPutMedia, pBuf, uLen)) != PUT_MEDIA_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed to send data frame");
        }
    }

    return res;
}

void print_escaped(const char *str, size_t len) {
    while (len--) {
        switch (*str) {
            case '\n': printf("\\n"); break;
            case '\t': printf("\\t"); break;
            case '\r': printf("\\r"); break;
            case '\b': printf("\\b"); break;
            case '\f': printf("\\f"); break;
            case '\\': printf("\\\\"); break;
            case '\"': printf("\\\""); break;
            case '\'': printf("\\\'"); break;
            default:
                putchar(*str);
        }
        str++;
    }
}

/*
 * Parse every fragment ack packed into pBuf and queue it for the caller.
 * Returns a PUTMEDIA error if one of the acks reports a session error.
 */
static int prvProcessAckBuffer(PutMedia_t *pPutMedia, const uint8_t *pBuf, int uLen)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    int uBytesConsumed = 0;
    FragmentAck_t xFragmentAck = {0};
    size_t uFragAckLen = 0;

    while (uBytesConsumed < uLen) {
        memset(&xFragmentAck, 0, sizeof(FragmentAck_t));
        if (prvParseFragmentAck((char *)pBuf + uBytesConsumed, uLen - uBytesConsumed, &xFragmentAck, &uFragAckLen) != PUT_MEDIA_ERRNO_NONE ||
                uFragAckLen == 0) {
            break;
        }

        prvLogFragmentAck(&xFragmentAck);
        prvPushFragmentAck(pPutMedia, &xFragmentAck);
        if (xFragmentAck.eventType == eError) {
            res = PUT_MEDIA_GENERATE_PUTMEDIA_ERROR(xFragmentAck.uErrorId);
            break;
        }

        uBytesConsumed += uFragAckLen;
    }

    return res;
}

int Kvs_putMediaDoWork(PutMediaHandle xPutMediaHandle)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    PutMedia_t *pPutMedia = xPutMediaHandle;
    int uBytesTotalReceived = 0;
    int uBytesReceived = 0;
    uint8_t buffer[2560];

    if (pPutMedia == NULL || pPutMedia->xHttpClient == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else {
        prvFlushFragmentAck(pPutMedia);

        /* After 100 Continue, must call fetch_headers() once to consume
         * the 200 OK headers before esp_http_client_read() can return acks. */
        if (!pPutMedia->bSecondResponseHeadersFetched) {
            if (esp_http_client_fetch_headers(pPutMedia->xHttpClient) < 0) {
                /* 200 not yet sent by server (e.g. no fragment sent yet) or error */
                return res;
            }
            pPutMedia->bSecondResponseHeadersFetched = 1;
        }

        /*
         * Use a very short timeout (1 ms) so the read is non-blocking,
         * matching the original esp_transport_read(transport, ..., 1)
         * behaviour.  esp_http_client_read() internally loops trying to
         * fill the whole buffer; with the client's default 2 s timeout
         * it would block the sending path and starve KVS of fragments,
         * which in turn means no new acks are generated.
         */
        esp_http_client_set_timeout_ms(pPutMedia->xHttpClient, 1);
        uBytesReceived = esp_http_client_read(pPutMedia->xHttpClient, (char *)buffer, sizeof(buffer));
        esp_http_client_set_timeout_ms(pPutMedia->xHttpClient, pPutMedia->uRecvTimeoutMs);

        if (uBytesReceived < 0) {
            if (uBytesReceived == -ESP_ERR_HTTP_EAGAIN) {
                uBytesTotalReceived = 0;
            } else {
                ESP_LOGE(TAG, "Failed to receive");
                res = PUT_MEDIA_ERROR_NETIO_UNABLE_TO_READ_CONTENT;
            }
        } else {
            uBytesTotalReceived = uBytesReceived;
        }

        if (res == PUT_MEDIA_ERRNO_NONE && uBytesTotalReceived > 0) {
            ESP_LOGD(TAG, "uBytesReceived: %d", uBytesTotalReceived);
            res = prvProcessAckBuffer(pPutMedia, buffer, uBytesTotalReceived);
            prvLogPendingFragmentAcks(pPutMedia);
        }
    }

    return res;
}

/*
 * Close the chunked request body and read whatever the service has left to say.
 * Without the terminator AWS sees a truncated PutMedia request and may drop the
 * final fragment, and any terminal error ack would be lost.
 */
static void prvFinishAndDrain(PutMedia_t *pPutMedia)
{
    uint8_t buffer[512];
    int i;

    if (Http_finishChunkedRequest(pPutMedia->xHttpClient) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGW(TAG, "Failed to send final chunk terminator");
        return;
    }

    esp_http_client_set_timeout_ms(pPutMedia->xHttpClient, PUT_MEDIA_DRAIN_TIMEOUT_MS);

    /* The 200 headers may still be pending if no ack arrived during the session. */
    if (!pPutMedia->bSecondResponseHeadersFetched &&
            esp_http_client_fetch_headers(pPutMedia->xHttpClient) >= 0) {
        pPutMedia->bSecondResponseHeadersFetched = 1;
    }

    for (i = 0; i < PUT_MEDIA_DRAIN_MAX_READS; i++) {
        int uBytesReceived = esp_http_client_read(pPutMedia->xHttpClient, (char *)buffer, sizeof(buffer));
        if (uBytesReceived <= 0) {
            break;
        }
        prvProcessAckBuffer(pPutMedia, buffer, uBytesReceived);
    }

    prvLogPendingFragmentAcks(pPutMedia);
}

void Kvs_putMediaFinish(PutMediaHandle xPutMediaHandle)
{
    PutMedia_t *pPutMedia = xPutMediaHandle;

    if (pPutMedia != NULL) {
        if (pPutMedia->xHttpClient != NULL) {
            /* A stream left mid-chunk by a failed write cannot be closed cleanly. */
            if (pPutMedia->bStreamCorrupted) {
                ESP_LOGW(TAG, "Dropping PutMedia connection without terminator, stream was corrupted");
            } else {
                prvFinishAndDrain(pPutMedia);
            }
            esp_http_client_close(pPutMedia->xHttpClient);
            esp_http_client_cleanup(pPutMedia->xHttpClient);
            pPutMedia->xHttpClient = NULL;
        }
        prvFlushFragmentAck(pPutMedia);
        Lock_Deinit(pPutMedia->xLock);
        kvsFree(pPutMedia);
    }
}

int Kvs_putMediaUpdateRecvTimeout(PutMediaHandle xPutMediaHandle, unsigned int uRecvTimeoutMs)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    PutMedia_t *pPutMedia = xPutMediaHandle;

    if (pPutMedia == NULL || pPutMedia->xHttpClient == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if ((res = esp_http_client_set_timeout_ms(pPutMedia->xHttpClient, uRecvTimeoutMs)) != ESP_OK){
        res = PUT_MEDIA_GENERATE_ESP_ERR_ERROR(res);
    } else {
        pPutMedia->uRecvTimeoutMs = uRecvTimeoutMs;
    }

    return res;
}

int Kvs_putMediaUpdateSendTimeout(PutMediaHandle xPutMediaHandle, unsigned int uSendTimeoutMs)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    PutMedia_t *pPutMedia = xPutMediaHandle;

    if (pPutMedia == NULL || pPutMedia->xHttpClient == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if ((res = esp_http_client_set_timeout_ms(pPutMedia->xHttpClient, uSendTimeoutMs)) != ESP_OK){
        res = PUT_MEDIA_GENERATE_ESP_ERR_ERROR(res);
        /* Propagate the res error */
    }

    return res;
}

int Kvs_putMediaReadFragmentAck(PutMediaHandle xPutMediaHandle, ePutMediaFragmentAckEventType *peAckEventType, uint64_t *puFragmentTimecode, unsigned int *puErrorId)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    PutMedia_t *pPutMedia = xPutMediaHandle;
    FragmentAck_t *pFragmentAck = NULL;

    if (pPutMedia == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else if ((pFragmentAck = prvReadFragmentAck(pPutMedia)) == NULL) {
        res = PUT_MEDIA_ERROR_NO_PUTMEDIA_FRAGMENT_ACK_AVAILABLE;
    } else {
        if (peAckEventType != NULL) {
            *peAckEventType = pFragmentAck->eventType;
        }
        if (puFragmentTimecode != NULL) {
            *puFragmentTimecode = pFragmentAck->uFragmentTimecode;
        }
        if (puErrorId != NULL) {
            *puErrorId = pFragmentAck->uErrorId;
        }
        kvsFree(pFragmentAck);
    }

    return res;
}

