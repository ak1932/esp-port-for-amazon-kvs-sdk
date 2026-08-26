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

/* Thirdparty headers */

/* Public headers */
#include "kvs/put_media_errors.h"
#include "kvs/iot_credential_provider.h"

/* Internal headers */
#include "os/allocator.h"
#include "net/http_helper.h"
#include "misc/json_helper.h"

#include "esp_log.h"

#define TAG "PUT_MEDIA_IOT_CREDS"

#define IOT_URI_ROLE_ALIASES_BEGIN  "/role-aliases"
#define IOT_URI_ROLE_ALIASES_END    "/credentials"
#define IOT_HTTP_TIMEOUT_MS         10000

/* "/role-aliases/<alias>/credentials"; AWS IoT caps a role alias at 128 characters. */
#define IOT_URI_MAX_LEN             192

static int parseIoTCredential(const char *pcJsonSrc, size_t uJsonSrcLen, IotCredentialToken_t *pToken)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    cJSON *pxRoot = NULL;

    if (pcJsonSrc == NULL || uJsonSrcLen == 0 || pToken == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if (
        (pxRoot = cJSON_ParseWithLength(pcJsonSrc, uJsonSrcLen)) == NULL ||
        (pToken->pAccessKeyId = json_dotget_string(pxRoot, "credentials.accessKeyId")) == NULL ||
        (pToken->pSecretAccessKey = json_dotget_string(pxRoot, "credentials.secretAccessKey")) == NULL ||
        (pToken->pSessionToken = json_dotget_string(pxRoot, "credentials.sessionToken")) == NULL) {
        res = PUT_MEDIA_ERROR_FAIL_TO_PARSE_JSON_OF_IOT_CREDENTIAL;
        ESP_LOGE(TAG, "Failed to parse IoT credential");
    } else {
        /* nop */
    }

    cJSON_Delete(pxRoot);

    return res;
}

IotCredentialToken_t *Iot_getCredential(IotCredentialRequest_t *pReq)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    IotCredentialToken_t *pToken = NULL;

    char pcUri[IOT_URI_MAX_LEN] = {0};

    unsigned int uHttpStatusCode = 0;
    HttpHeaders_t *xHttpReqHeaders = NULL;
    char *pRspBody = NULL;
    size_t uRspBodyLen = 0;

    if (pReq == NULL || pReq->pCredentialHost == NULL || pReq->pRoleAlias == NULL || pReq->pThingName == NULL || pReq->pRootCA == NULL || pReq->pCertificate == NULL ||
            pReq->pPrivateKey == NULL) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if (snprintf(pcUri, sizeof(pcUri), "%s/%s%s", IOT_URI_ROLE_ALIASES_BEGIN, pReq->pRoleAlias, IOT_URI_ROLE_ALIASES_END) >= (int)sizeof(pcUri)) {
        res = PUT_MEDIA_ERROR_STRING_ERROR;
        ESP_LOGE(TAG, "Role alias too long for the IoT URI");
    } else if (
        (xHttpReqHeaders = HttpHeaders_create()) == NULL || HttpHeaders_add(xHttpReqHeaders, HDR_HOST, pReq->pCredentialHost) != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, "accept", "*/*") != PUT_MEDIA_ERRNO_NONE ||
        HttpHeaders_add(xHttpReqHeaders, HDR_X_AMZN_IOT_THINGNAME, pReq->pThingName) != PUT_MEDIA_ERRNO_NONE) {
        res = PUT_MEDIA_ERROR_FAIL_TO_GENERATE_HTTP_HEADERS;
        ESP_LOGE(TAG, "Failed to generate HTTP headers");
    } else if ((res = Http_performRequestWithX509(pReq->pCredentialHost, "443", pcUri, HTTP_METHOD_GET,
                xHttpReqHeaders, HTTP_BODY_EMPTY, IOT_HTTP_TIMEOUT_MS, IOT_HTTP_TIMEOUT_MS,
                pReq->pRootCA, pReq->pCertificate, pReq->pPrivateKey,
                &uHttpStatusCode, &pRspBody, &uRspBodyLen)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed http request to %s", pReq->pCredentialHost);
        /* Propagate the res error */
    } else {
        if (uHttpStatusCode != 200) {
            ESP_LOGI(TAG, "Get IoT credential failed, HTTP status code: %u", uHttpStatusCode);
            ESP_LOGI(TAG, "HTTP response message:%.*s", (int)uRspBodyLen, pRspBody);
        } else {
            if ((pToken = (IotCredentialToken_t *)kvsMalloc(sizeof(IotCredentialToken_t))) == NULL) {
                res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
                ESP_LOGE(TAG, "OOM: pToken");
            } else {
                memset(pToken, 0, sizeof(IotCredentialToken_t));
                if ((res = parseIoTCredential(pRspBody, uRspBodyLen, pToken)) != PUT_MEDIA_ERRNO_NONE) {
                    ESP_LOGE(TAG, "Failed to parse IoT credential token");
                    /* Propagate the res error */
                }
            }
        }
    }

    if (res != PUT_MEDIA_ERRNO_NONE) {
        Iot_credentialTerminate(pToken);
        pToken = NULL;
    }

    if (pRspBody != NULL) {
        kvsFree(pRspBody);
    }

    HttpHeaders_free(xHttpReqHeaders);

    return pToken;
}

void Iot_credentialTerminate(IotCredentialToken_t *pToken)
{
    if (pToken != NULL) {
        if (pToken->pAccessKeyId != NULL) {
            kvsFree(pToken->pAccessKeyId);
        }
        if (pToken->pSecretAccessKey != NULL) {
            kvsFree(pToken->pSecretAccessKey);
        }
        if (pToken->pSessionToken != NULL) {
            kvsFree(pToken->pSessionToken);
        }
        kvsFree(pToken);
    }
}
