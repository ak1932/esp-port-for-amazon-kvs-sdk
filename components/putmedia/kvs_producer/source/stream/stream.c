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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Third party headers */
#include "azure_c_shared_utility/doublylinkedlist.h"
#include "azure_c_shared_utility/lock.h"

/* Public headers */
#include "kvs/mkv_errors.h"
#include "kvs/mkv_generator.h"
#include "kvs/stream.h"

/* Internal headers */
#include "os/allocator.h"

#include "esp_log.h"

#define TAG "MKV_STREAM"

typedef struct DataFrame {
    DataFrameIn_t xDataFrameIn;

    DLIST_ENTRY xClusterEntry;
    DLIST_ENTRY xDataFrameEntry;

    size_t uMkvHdrLen;
    char *pMkvHdr;
} DataFrame_t;

typedef struct Stream {
    LOCK_HANDLE xLock;

    char *pMkvEbmlSeg;
    size_t uMkvEbmlSegLen;

    uint64_t uEarliestClusterTimestamp;
    DLIST_ENTRY xClusterPending;
    DLIST_ENTRY xDataFramePending;

    bool bHasVideoTrack;
    bool bHasAudioTrack;
} Stream_t;

static DataFrameHandle prvStreamPop(StreamHandle xStreamHandle, bool bPeek)
{
    Stream_t *pxStream = xStreamHandle;
    DataFrame_t *pxDataFrame = NULL;
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;

    if (pxStream == NULL) {
        ESP_LOGE(TAG, "invalid argument");
    } else {
        if (Lock(pxStream->xLock) != LOCK_OK) {
            ESP_LOGE(TAG, "Failed to Lock");
        } else {
            if (DList_IsListEmpty(&(pxStream->xDataFramePending))) {
                /* ESP_LOGI(TAG,"No data frame to pop"); */
            } else {
                pxListHead = &(pxStream->xDataFramePending);

                if (!bPeek) {
                    pxListItem = DList_RemoveHeadList(pxListHead);
                } else {
                    pxListItem = pxListHead->Flink;
                }
                pxDataFrame = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);

                if (!bPeek) {
                    if (pxDataFrame->xDataFrameIn.xClusterType == MKV_CLUSTER) {
                        pxStream->uEarliestClusterTimestamp = pxDataFrame->xDataFrameIn.uTimestampMs;
                    }
                }
            }

            Unlock(pxStream->xLock);
        }
    }

    return pxDataFrame;
}

StreamHandle Kvs_streamCreate(VideoTrackInfo_t *pVideoTrackInfo, AudioTrackInfo_t *pAudioTrackInfo)
{
    Stream_t *pxStream = NULL;
    MkvHeader_t xMkvHeader = {0};

    if (pVideoTrackInfo == NULL) {
        ESP_LOGE(TAG, "Invalid argument");
    } else if ((pxStream = (Stream_t *)kvsMalloc(sizeof(Stream_t))) == NULL) {
        ESP_LOGE(TAG, "OOM: pxStream");
    } else {
        ESP_LOGI(TAG, "created stream buffer");
        memset(pxStream, 0, sizeof(Stream_t));

        DList_InitializeListHead(&(pxStream->xClusterPending));
        DList_InitializeListHead(&(pxStream->xDataFramePending));

        if (Mkv_initializeHeaders(&xMkvHeader, pVideoTrackInfo, pAudioTrackInfo) != MKV_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed to initialize mkv headers");
            kvsFree(pxStream);
            pxStream = NULL;
        } else if ((pxStream->xLock = Lock_Init()) == NULL) {
            ESP_LOGE(TAG, "Failed to initialize lock");
            kvsFree(pxStream);
            pxStream = NULL;
        } else {
            pxStream->pMkvEbmlSeg = (char *)(xMkvHeader.pHeader);
            pxStream->uMkvEbmlSegLen = (size_t)(xMkvHeader.uHeaderLen);
            pxStream->bHasVideoTrack = true;
            pxStream->bHasAudioTrack = (pAudioTrackInfo == NULL) ? false : true;
        }
    }

    return pxStream;
}

void Kvs_streamTermintate(StreamHandle xStreamHandle)
{
    Stream_t *pxStream = xStreamHandle;

    if (pxStream != NULL) {
        kvsFree(pxStream->pMkvEbmlSeg);
        Lock_Deinit(pxStream->xLock);
        kvsFree(pxStream);
    }
}

int Kvs_streamGetMkvEbmlSegHdr(StreamHandle xStreamHandle, uint8_t **ppMkvHeader, size_t *puMkvHeaderLen)
{
    int res = MKV_ERRNO_NONE;
    Stream_t *pxStream = xStreamHandle;

    if (pxStream == NULL || ppMkvHeader == NULL || puMkvHeaderLen == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if (pxStream->pMkvEbmlSeg == NULL || pxStream->uMkvEbmlSegLen == 0) {
        res = MKV_ERROR_STREAM_MKV_IS_NOT_INITIALIZED;
        ESP_LOGE(TAG, "Mkv EBML and segment are not initialized");
    } else {
        *ppMkvHeader = (uint8_t *)(pxStream->pMkvEbmlSeg);
        *puMkvHeaderLen = pxStream->uMkvEbmlSegLen;
    }

    return res;
}

DataFrameHandle Kvs_streamAddDataFrame(StreamHandle xStreamHandle, DataFrameIn_t *pxDataFrameIn)
{
    int res = MKV_ERRNO_NONE;
    Stream_t *pxStream = xStreamHandle;
    DataFrame_t *pxDataFrame = NULL;
    size_t uMkvHdrLen = 0;
    DataFrame_t *pxDataFrameCurrent = NULL;
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;
    bool bListAdded = false;
    bool bNeedCorrectDeltaTimestamp = false;
    bool bCorrectDeltaTimestampStarted = false;
    uint64_t uClusterTimestamp = 0;
    uint16_t uDeltaTimestampMs = 0;

    if (pxStream == NULL || pxDataFrameIn == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if ((uMkvHdrLen = Mkv_getClusterHdrLen(pxDataFrameIn->xClusterType)) == 0) {
        res = MKV_ERROR_INVALID_CLUSTER_HDR_LEN;
        ESP_LOGE(TAG, "Invalid cluster len");
    } else if ((pxDataFrame = (DataFrame_t *)kvsMalloc(sizeof(DataFrame_t))) == NULL) {
        res = MKV_ERROR_OUT_OF_MEMORY;
        ESP_LOGE(TAG, "OOM: pxDataFrame");
    } else if (memset(pxDataFrame, 0, sizeof(DataFrame_t)) == NULL) {
        /* noop */
    } else if ((pxDataFrame->pMkvHdr = kvsMalloc(uMkvHdrLen)) == NULL) {
        res = MKV_ERROR_OUT_OF_MEMORY;
        ESP_LOGE(TAG, "OOM: pxDataFrame->pMkvHeader");
    } else if (Lock(pxStream->xLock) != LOCK_OK) {
        res = MKV_ERROR_LOCK_ERROR;
        ESP_LOGE(TAG, "Failed to Lock");
    } else {
        memcpy(pxDataFrame, pxDataFrameIn, sizeof(DataFrameIn_t));
        DList_InitializeListHead(&(pxDataFrame->xClusterEntry));
        DList_InitializeListHead(&(pxDataFrame->xDataFrameEntry));
        pxDataFrame->uMkvHdrLen = uMkvHdrLen;
        uClusterTimestamp = pxStream->uEarliestClusterTimestamp;

        pxListHead = &(pxStream->xDataFramePending);
        pxListItem = pxListHead->Flink;
        while (pxListItem != pxListHead) {
            pxDataFrameCurrent = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);
            if (pxDataFrame->xDataFrameIn.uTimestampMs < pxDataFrameCurrent->xDataFrameIn.uTimestampMs ||
                    ((pxDataFrame->xDataFrameIn.uTimestampMs == pxDataFrameCurrent->xDataFrameIn.uTimestampMs) && (pxDataFrame->xDataFrameIn.xTrackType == TRACK_VIDEO))) {
                DList_InsertTailList(pxListItem, &(pxDataFrame->xDataFrameEntry));
                uDeltaTimestampMs = (uint16_t)(pxDataFrame->xDataFrameIn.uTimestampMs - uClusterTimestamp);
                if (pxDataFrame->xDataFrameIn.xClusterType == MKV_CLUSTER) {
                    uDeltaTimestampMs = 0;

                    /* If we insert a cluster head, then the following frames's delta timestamp needs updates. */
                    bNeedCorrectDeltaTimestamp = true;
                }
                bListAdded = true;
                break;
            }
            if (pxDataFrameCurrent->xDataFrameIn.xClusterType == MKV_CLUSTER) {
                uClusterTimestamp = pxDataFrameCurrent->xDataFrameIn.uTimestampMs;
            }
            pxListItem = pxListItem->Flink;
        }
        if (!bListAdded) {
            uDeltaTimestampMs = (uint16_t)(pxDataFrame->xDataFrameIn.uTimestampMs - uClusterTimestamp);
            DList_InsertTailList(&(pxStream->xDataFramePending), &(pxDataFrame->xDataFrameEntry));
            bListAdded = true;
        }

        Mkv_initializeClusterHdr(
            (uint8_t *)(pxDataFrame->pMkvHdr),
            pxDataFrame->uMkvHdrLen,
            pxDataFrameIn->xClusterType,
            pxDataFrameIn->uDataLen,
            pxDataFrameIn->xTrackType,
            pxDataFrameIn->bIsKeyFrame,
            pxDataFrameIn->uTimestampMs,
            uDeltaTimestampMs);

        if (bNeedCorrectDeltaTimestamp) {
            bCorrectDeltaTimestampStarted = false;
            pxListHead = &(pxStream->xDataFramePending);
            pxListItem = pxListHead->Flink;
            while (pxListItem != pxListHead) {
                pxDataFrameCurrent = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);
                if (pxDataFrameCurrent->xDataFrameIn.xClusterType == MKV_CLUSTER) {
                    uClusterTimestamp = pxDataFrameCurrent->xDataFrameIn.uTimestampMs;
                    bCorrectDeltaTimestampStarted = true;
                }
                if (bCorrectDeltaTimestampStarted) {
                    uDeltaTimestampMs = (uint16_t)(pxDataFrameCurrent->xDataFrameIn.uTimestampMs - uClusterTimestamp);
                    Mkv_initializeClusterHdr(
                        (uint8_t *)(pxDataFrameCurrent->pMkvHdr),
                        pxDataFrameCurrent->uMkvHdrLen,
                        pxDataFrameCurrent->xDataFrameIn.xClusterType,
                        pxDataFrameCurrent->xDataFrameIn.uDataLen,
                        pxDataFrameCurrent->xDataFrameIn.xTrackType,
                        pxDataFrameCurrent->xDataFrameIn.bIsKeyFrame,
                        pxDataFrameCurrent->xDataFrameIn.uTimestampMs,
                        uDeltaTimestampMs);
                }
                pxListItem = pxListItem->Flink;
            }
        }

        Unlock(pxStream->xLock);
    }

    if (res != MKV_ERRNO_NONE) {
        if (pxDataFrame != NULL) {
            if (pxDataFrame->pMkvHdr != NULL) {
                kvsFree(pxDataFrame->pMkvHdr);
            }
            kvsFree(pxDataFrame);
            pxDataFrame = NULL;
        }
    }

    return pxDataFrame;
}

DataFrameHandle Kvs_streamPop(StreamHandle xStreamHandle)
{
    return prvStreamPop(xStreamHandle, false);
}

DataFrameHandle Kvs_streamPeek(StreamHandle xStreamHandle)
{
    return prvStreamPop(xStreamHandle, true);
}

bool Kvs_streamIsEmpty(StreamHandle xStreamHandle)
{
    bool bRes = true;
    Stream_t *pxStream = xStreamHandle;

    if (pxStream != NULL) {
        if (Lock(pxStream->xLock) != LOCK_OK) {
            ESP_LOGE(TAG, "Failed to Lock");
        } else {
            if (!DList_IsListEmpty(&(pxStream->xDataFramePending))) {
                bRes = false;
            }
            Unlock(pxStream->xLock);
        }
    }

    return bRes;
}

bool Kvs_streamAvailOnTrack(StreamHandle xStreamHandle, TrackType_t xTrackType)
{
    bool bRes = false;
    Stream_t *pxStream = xStreamHandle;
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;
    DataFrame_t *pxDataFrame = NULL;

    if (pxStream != NULL) {
        if (Lock(pxStream->xLock) != LOCK_OK) {
            ESP_LOGE(TAG, "Failed to Lock");
        } else {
            pxListHead = &(pxStream->xDataFramePending);
            pxListItem = pxListHead->Flink;
            while (pxListItem != pxListHead) {
                pxDataFrame = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);
                if (pxDataFrame->xDataFrameIn.xTrackType == xTrackType) {
                    bRes = true;
                    break;
                }

                pxListItem = pxListItem->Flink;
            }

            Unlock(pxStream->xLock);
        }
    }

    return bRes;
}

int Kvs_streamMemStatTotal(StreamHandle xStreamHandle, size_t *puMemTotal)
{
    int res = MKV_ERRNO_NONE;
    Stream_t *pxStream = xStreamHandle;
    DataFrame_t *pxDataFrame = NULL;
    PDLIST_ENTRY pxListHead = NULL;
    PDLIST_ENTRY pxListItem = NULL;
    size_t uMemTotal = 0;

    if (pxStream == NULL || puMemTotal == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else if (Lock(pxStream->xLock) != LOCK_OK) {
        res = MKV_ERROR_LOCK_ERROR;
        ESP_LOGE(TAG, "Failed to Lock");
    } else {
        uMemTotal += sizeof(Stream_t) + pxStream->uMkvEbmlSegLen;

        pxListHead = &(pxStream->xDataFramePending);
        pxListItem = pxListHead->Flink;
        while (pxListItem != pxListHead) {
            pxDataFrame = containingRecord(pxListItem, DataFrame_t, xDataFrameEntry);
            uMemTotal += pxDataFrame->xDataFrameIn.uDataLen;
            uMemTotal += sizeof(DataFrame_t) + pxDataFrame->uMkvHdrLen;
            pxListItem = pxListItem->Flink;
        }

        *puMemTotal = uMemTotal;
        Unlock(pxStream->xLock);
    }

    return res;
}

int Kvs_dataFrameGetContent(DataFrameHandle xDataFrameHandle, uint8_t **ppMkvHeader, size_t *puMkvHeaderLen, uint8_t **ppData, size_t *puDataLen)
{
    int res = MKV_ERRNO_NONE;
    DataFrame_t *pxDataFrame = xDataFrameHandle;

    if (pxDataFrame == NULL || ppMkvHeader == NULL || puMkvHeaderLen == NULL || ppData == NULL || puDataLen == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid argument");
    } else {
        *ppMkvHeader = (uint8_t *)(pxDataFrame->pMkvHdr);
        *puMkvHeaderLen = pxDataFrame->uMkvHdrLen;
        *ppData = (uint8_t *)(pxDataFrame->xDataFrameIn.pData);
        *puDataLen = pxDataFrame->xDataFrameIn.uDataLen;
    }

    return res;
}

int Kvs_dataFrameAddTags(DataFrameHandle xDataFrameHandle, MkvTag_t* tagsList, size_t tagsListLen, bool endOfStream, uint8_t **ppMkvHeader, size_t *puMkvHeaderLen, uint8_t **ppData, size_t *puDataLen)
{
    int res = MKV_ERRNO_NONE;
    DataFrame_t *pxDataFrame = xDataFrameHandle;

    static bool firstClusterSeen = false;

    // Input validation
    if (pxDataFrame == NULL || ppMkvHeader == NULL ||
            puMkvHeaderLen == NULL || ppData == NULL || puDataLen == NULL) {
        // ESP_LOGE(TAG,"Invalid argument in Kvs_dataFrameAddTags");
        return MKV_ERROR_INVALID_ARGUMENT;
    }

    // pull request
    if (pxDataFrame->xDataFrameIn.xClusterType != MKV_CLUSTER) {
        // ESP_LOGI(TAG,"Not a cluster");
        return MKV_ERRNO_NONE;
    }

    size_t effectiveTagsLen = tagsListLen + (endOfStream ? 1 : 0);
    if (effectiveTagsLen >= MAX_TAG_AMOUNT) {
        ESP_LOGE(TAG, "Trying to add too many tags - Adding: %zu tags, Max: %d", effectiveTagsLen, MAX_TAG_AMOUNT);
        return MKV_ERROR_INVALID_ARGUMENT;
    }

    // Track cluster transitions
    if (!firstClusterSeen) {
        firstClusterSeen = true;
        // ESP_LOGI(TAG,"First cluster detected - skipping tags");
    } else {
        // ESP_LOGI(TAG,"Adding tags before cluster");

        // Get the original header information
        uint8_t *originalHeader = (uint8_t *)pxDataFrame->pMkvHdr;
        size_t originalHeaderLen = pxDataFrame->uMkvHdrLen;

        MkvTag_t tagsToAddBuffer[MAX_TAG_AMOUNT];
        MkvTag_t* tagsToAdd;
        size_t tagsToAddLen;

        if (endOfStream) {
            memcpy(tagsToAddBuffer, tagsList, tagsListLen * sizeof(MkvTag_t));

            MkvTag_t endOfFragmentTag;
            snprintf(endOfFragmentTag.key, MAX_TAG_NAME_LEN, "AWS_KINESISVIDEO_END_OF_FRAGMENT");
            endOfFragmentTag.value[0] = '\0';

            tagsToAddBuffer[tagsListLen] = endOfFragmentTag;

            tagsToAdd = tagsToAddBuffer;
            tagsToAddLen = tagsListLen + 1;
        } else {
            tagsToAdd = tagsList;
            tagsToAddLen = tagsListLen;
        }

        for (size_t i = 0; i < tagsToAddLen; i++) {
            // ESP_LOGI(TAG,"Adding tag: key=%s, val=%s", tagsToAdd[i].key, tagsToAdd[i].value);
        }

        // Generate tags header
        MkvTagsBuffer_t tagsBuffer;
        if (Mkv_generateTags(tagsToAdd, tagsToAddLen, &tagsBuffer) != MKV_ERRNO_NONE || tagsBuffer.buffer == NULL) {
            ESP_LOGE(TAG, "Failed to create MKV tags header");
            return MKV_ERROR_OUT_OF_MEMORY;
        }

        // Allocate and create new combined header
        size_t newHeaderLen = tagsBuffer.size + originalHeaderLen;
        uint8_t *newHeader = (uint8_t *)malloc(newHeaderLen);
        if (newHeader == NULL) {
            free(tagsBuffer.buffer);
            ESP_LOGE(TAG, "Failed to allocate memory for new header");
            return MKV_ERROR_OUT_OF_MEMORY;
        }

        // Combine headers
        memcpy(newHeader, tagsBuffer.buffer, tagsBuffer.size);
        memcpy(newHeader + tagsBuffer.size, originalHeader, originalHeaderLen);

        // Clean up tags header buffer
        free(tagsBuffer.buffer);
        free(originalHeader);

        // Update the frame's header information
        pxDataFrame->pMkvHdr = (char *)newHeader;
        pxDataFrame->uMkvHdrLen = newHeaderLen;
    }

    // Assign frame content
    *ppMkvHeader = (uint8_t *)(pxDataFrame->pMkvHdr);
    *puMkvHeaderLen = pxDataFrame->uMkvHdrLen;
    *ppData = (uint8_t *)(pxDataFrame->xDataFrameIn.pData);
    *puDataLen = pxDataFrame->xDataFrameIn.uDataLen;

    return res;
}

void Kvs_dataFrameTerminate(DataFrameHandle xDataFrameHandle)
{
    DataFrame_t *pxDataFrame = xDataFrameHandle;

    if (pxDataFrame != NULL) {
        if (pxDataFrame->pMkvHdr != NULL) {
            kvsFree(pxDataFrame->pMkvHdr);
        }
        kvsFree(pxDataFrame);
    }
}
