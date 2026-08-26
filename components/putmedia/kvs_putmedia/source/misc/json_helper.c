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
#include <stdlib.h>
#include <string.h>

/* Internal headers */
#include "misc/json_helper.h"
#include "os/allocator.h"

/* Longest key any caller looks up is "FragmentTimecode". */
#define JSON_MAX_KEY_LEN 64

/**
 * @brief Copy a NUL-terminated string into allocator memory.
 *
 * Values handed back to callers are freed with kvsFree, so they cannot come
 * straight out of cJSON's own allocator.
 */
static char *prvDupToKvs(const char *pcVal)
{
    char *pcRes = NULL;
    size_t uLen = 0;

    if (pcVal != NULL && (uLen = strlen(pcVal)) > 0 && (pcRes = (char *)kvsMalloc(uLen + 1)) != NULL) {
        memcpy(pcRes, pcVal, uLen + 1);
    }

    return pcRes;
}

/**
 * @brief Walk a dot-separated path from the root object.
 */
static const cJSON *prvDotGet(const cJSON *pxRoot, const char *pcPath)
{
    const cJSON *pxItem = pxRoot;
    const char *pcSeg = pcPath;
    const char *pcDot = NULL;
    char pcKey[JSON_MAX_KEY_LEN] = {0};
    size_t uSegLen = 0;

    if (pxItem == NULL || pcSeg == NULL || *pcSeg == '\0') {
        return NULL;
    }

    while (pcSeg != NULL) {
        pcDot = strchr(pcSeg, '.');
        uSegLen = (pcDot != NULL) ? (size_t)(pcDot - pcSeg) : strlen(pcSeg);

        if (uSegLen == 0 || uSegLen >= sizeof(pcKey)) {
            return NULL;
        }
        memcpy(pcKey, pcSeg, uSegLen);
        pcKey[uSegLen] = '\0';

        if ((pxItem = cJSON_GetObjectItemCaseSensitive(pxItem, pcKey)) == NULL) {
            return NULL;
        }

        pcSeg = (pcDot != NULL) ? pcDot + 1 : NULL;
    }

    return pxItem;
}

char *json_dotget_string(const cJSON *pxRoot, const char *pcPath)
{
    const cJSON *pxItem = prvDotGet(pxRoot, pcPath);
    char *pcSerialized = NULL;
    char *pcRes = NULL;

    if (pxItem == NULL) {
        /* noop */
    } else if (cJSON_IsString(pxItem)) {
        pcRes = prvDupToKvs(cJSON_GetStringValue(pxItem));
    } else if ((pcSerialized = cJSON_PrintUnformatted(pxItem)) != NULL) {
        pcRes = prvDupToKvs(pcSerialized);
        cJSON_free(pcSerialized);
    }

    return pcRes;
}

uint64_t json_dotget_uint64(const cJSON *pxRoot, const char *pcPath, int xBase)
{
    const cJSON *pxItem = prvDotGet(pxRoot, pcPath);
    uint64_t uVal = 0;

    if (pxItem == NULL) {
        /* noop */
    } else if (cJSON_IsNumber(pxItem)) {
        /* KVS sends epoch-millisecond timecodes, well inside the 2^53 a double holds exactly. */
        if (pxItem->valuedouble > 0.0) {
            uVal = (uint64_t)pxItem->valuedouble;
        }
    } else if (cJSON_IsString(pxItem) && pxItem->valuestring != NULL) {
        uVal = strtoull(pxItem->valuestring, NULL, xBase);
    }

    return uVal;
}
