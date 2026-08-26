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
#include <string.h>
#include <strings.h>

/* Public headers */
#include "kvs/put_media_errors.h"

/* Internal headers */
#include "os/allocator.h"
#include "net/http_headers.h"

/* A request never carries more than a dozen headers; grow in small steps. */
#define HTTP_HEADERS_GROW_BY 8

typedef struct HttpHeader {
    char *pcName;
    char *pcValue;
} HttpHeader_t;

struct HttpHeaders {
    HttpHeader_t *pxItems;
    size_t uCount;
    size_t uCapacity;
};

static char *prvStrDup(const char *pcSrc)
{
    char *pcRes = NULL;
    size_t uLen = 0;

    if (pcSrc != NULL) {
        uLen = strlen(pcSrc);
        if ((pcRes = (char *)kvsMalloc(uLen + 1)) != NULL) {
            memcpy(pcRes, pcSrc, uLen + 1);
        }
    }

    return pcRes;
}

HttpHeaders_t *HttpHeaders_create(void)
{
    return (HttpHeaders_t *)kvsCalloc(1, sizeof(HttpHeaders_t));
}

void HttpHeaders_free(HttpHeaders_t *pxHeaders)
{
    size_t i = 0;

    if (pxHeaders != NULL) {
        for (i = 0; i < pxHeaders->uCount; i++) {
            kvsFree(pxHeaders->pxItems[i].pcName);
            kvsFree(pxHeaders->pxItems[i].pcValue);
        }
        kvsFree(pxHeaders->pxItems);
        kvsFree(pxHeaders);
    }
}

int HttpHeaders_add(HttpHeaders_t *pxHeaders, const char *pcName, const char *pcValue)
{
    int res = PUT_MEDIA_ERRNO_NONE;
    HttpHeader_t *pxItems = NULL;
    char *pcNewName = NULL;
    char *pcNewValue = NULL;
    size_t uNewCapacity = 0;
    size_t i = 0;

    if (pxHeaders == NULL || pcName == NULL || pcValue == NULL) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

    if ((pcNewValue = prvStrDup(pcValue)) == NULL) {
        return PUT_MEDIA_ERROR_OUT_OF_MEMORY;
    }

    /* Replace in place if the header is already present. */
    for (i = 0; i < pxHeaders->uCount; i++) {
        if (strcasecmp(pxHeaders->pxItems[i].pcName, pcName) == 0) {
            kvsFree(pxHeaders->pxItems[i].pcValue);
            pxHeaders->pxItems[i].pcValue = pcNewValue;
            return PUT_MEDIA_ERRNO_NONE;
        }
    }

    if (pxHeaders->uCount == pxHeaders->uCapacity) {
        uNewCapacity = pxHeaders->uCapacity + HTTP_HEADERS_GROW_BY;
        if ((pxItems = (HttpHeader_t *)kvsRealloc(pxHeaders->pxItems, uNewCapacity * sizeof(HttpHeader_t))) == NULL) {
            res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
        } else {
            pxHeaders->pxItems = pxItems;
            pxHeaders->uCapacity = uNewCapacity;
        }
    }

    if (res == PUT_MEDIA_ERRNO_NONE && (pcNewName = prvStrDup(pcName)) == NULL) {
        res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
    }

    if (res != PUT_MEDIA_ERRNO_NONE) {
        kvsFree(pcNewValue);
    } else {
        pxHeaders->pxItems[pxHeaders->uCount].pcName = pcNewName;
        pxHeaders->pxItems[pxHeaders->uCount].pcValue = pcNewValue;
        pxHeaders->uCount++;
    }

    return res;
}

const char *HttpHeaders_find(const HttpHeaders_t *pxHeaders, const char *pcName)
{
    size_t i = 0;

    if (pxHeaders != NULL && pcName != NULL) {
        for (i = 0; i < pxHeaders->uCount; i++) {
            if (strcasecmp(pxHeaders->pxItems[i].pcName, pcName) == 0) {
                return pxHeaders->pxItems[i].pcValue;
            }
        }
    }

    return NULL;
}

size_t HttpHeaders_count(const HttpHeaders_t *pxHeaders)
{
    return (pxHeaders != NULL) ? pxHeaders->uCount : 0;
}

int HttpHeaders_get(const HttpHeaders_t *pxHeaders, size_t uIndex, const char **ppcName, const char **ppcValue)
{
    if (pxHeaders == NULL || ppcName == NULL || ppcValue == NULL || uIndex >= pxHeaders->uCount) {
        return PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    }

    *ppcName = pxHeaders->pxItems[uIndex].pcName;
    *ppcValue = pxHeaders->pxItems[uIndex].pcValue;

    return PUT_MEDIA_ERRNO_NONE;
}
