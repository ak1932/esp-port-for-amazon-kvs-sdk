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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Third-party headers */
#include "FreeRTOSConfig.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/lock.h"
#include "sdkconfig.h"

/* KVS headers */
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "kvs/mkv_errors.h"
#include "kvs/nalu.h"
#include "kvs/port.h"
#include "kvs/stream.h"

#include "kvs/mkvapp.h"

/* Internal headers */
#include "os/allocator.h"

#define TAG "MKVAPP"

#define VIDEO_CODEC_NAME "V_MPEG4/ISO/AVC"
#define VIDEO_TRACK_NAME "kvs video track"

#define DEFAULT_RING_BUFFER_MEM_LIMIT (1 * 1024 * 1024)
#define MKV_MUX_TASK_STACK_SIZE       (8 * 1024)

typedef struct PolicyRingBufferParameter {
    size_t uMemLimit;
} PolicyRingBufferParameter_t;

typedef struct StreamStrategy {
    mkv_app_stream_policy_t xPolicy;
    union {
        PolicyRingBufferParameter_t xRingBufferPara;
    };
} StreamStrategy_t;

typedef struct mkv_app {
    /* KVS streaming variables */
    uint64_t uEarliestTimestamp;
    StreamHandle xStreamHandle;
    bool isEbmlHeaderUpdated;
    StreamStrategy_t xStrategy;

    /* Track information */
    VideoTrackInfo_t *pVideoTrackInfo;
    uint8_t *pSps;
    size_t uSpsLen;
    uint8_t *pPps;
    size_t uPpsLen;

    bool isAudioTrackPresent;
    AudioTrackInfo_t *pAudioTrackInfo;

    MkvTag_t* tagsList;
    size_t tagsListLen;

    uint8_t onMkvGeneratedCallbacks;
    /* Session scope callbacks */
    on_mkv_generated_callback_info_t *onMkvGeneratedCallbackInfos;
    TaskHandle_t MkvApp_task_handle;
    uint64_t stream_start_time;

    enum {
        TASK_DEINITIATED = 0,
        TASK_INITIATED,
        TASK_STARTED,
        TASK_STOPPED,
    } state;
} mkv_app_t;

typedef struct data_frame_user_data {
    data_frame_callbacks_t xCallbacks;
} data_frame_user_data_t;

enum {
    START_TASK_SIGNAL = (1 << 1),
    STOP_TASK_SIGNAL  = (1 << 2)
};

mkv_err_t mkv_app_do_work(mkv_app_handle handle);

/**
 * Default implementation of OnDataFrameTerminateCallback_t. It calls free() to pData to release resource.
 *
 * @param[in] pData Pointer of data frame
 * @param[in] uDataLen Size of data frame
 * @param[in] uTimestamp Timestamp of data frame
 * @param[in] xTrackType Track type of data frame
 * @param[in] pAppData Pointer of application data that is assigned in function MkvApp_addFrameWithCallbacks()
 * @return 0 on success, non-zero value otherwise
 */
static int defaultOnDataFrameTerminate(uint8_t *pData, size_t uDataLen, uint64_t uTimestamp, TrackType_t xTrackType, void *pAppData)
{
    if (pData != NULL) {
        /* NOTE: this frame is not allocated from KVS, so it should not be freed by kvsFree() */
        free(pData);
    }

    return MKV_ERRNO_NONE;
}

static void prvCallOnDataFrameTerminate(DataFrameIn_t *pDataFrameIn)
{
    data_frame_user_data_t *pUserData = NULL;
    on_data_frame_terminate_info_t *pOnDataFrameTerminateCallbackInfo = NULL;

    if (pDataFrameIn != NULL) {
        pUserData = (data_frame_user_data_t *)pDataFrameIn->pUserData;
        if (pUserData != NULL) {
            pOnDataFrameTerminateCallbackInfo = &(pUserData->xCallbacks.onDataFrameTerminateInfo);
            if (pOnDataFrameTerminateCallbackInfo->onDataFrameTerminate != NULL) {
                pOnDataFrameTerminateCallbackInfo->onDataFrameTerminate((uint8_t *)(pDataFrameIn->pData), pDataFrameIn->uDataLen, pDataFrameIn->uTimestampMs, pDataFrameIn->xTrackType, pOnDataFrameTerminateCallbackInfo->pAppData);
            }
        }
    }
}

static void prvVideoTrackInfoTerminate(VideoTrackInfo_t *pVideoTrackInfo)
{
    if (pVideoTrackInfo != NULL) {
        if (pVideoTrackInfo->pTrackName != NULL) {
            kvsFree(pVideoTrackInfo->pTrackName);
        }
        if (pVideoTrackInfo->pCodecName != NULL) {
            kvsFree(pVideoTrackInfo->pCodecName);
        }
        if (pVideoTrackInfo->pCodecPrivate != NULL) {
            kvsFree(pVideoTrackInfo->pCodecPrivate);
        }
        memset(pVideoTrackInfo, 0, sizeof(VideoTrackInfo_t));
        kvsFree(pVideoTrackInfo);
    }
}

static void prvAudioTrackInfoTerminate(AudioTrackInfo_t *pAudioTrackInfo)
{
    if (pAudioTrackInfo != NULL) {
        if (pAudioTrackInfo->pTrackName != NULL) {
            kvsFree(pAudioTrackInfo->pTrackName);
        }
        if (pAudioTrackInfo->pCodecName != NULL) {
            kvsFree(pAudioTrackInfo->pCodecName);
        }
        if (pAudioTrackInfo->pCodecPrivate != NULL) {
            kvsFree(pAudioTrackInfo->pCodecPrivate);
        }
        memset(pAudioTrackInfo, 0, sizeof(VideoTrackInfo_t));
        kvsFree(pAudioTrackInfo);
    }
}

static int prvMallocAndStrcpyHelper(char **destination, const char *source)
{
    int res = MKV_ERRNO_NONE;

    if ((destination == NULL) || (source == NULL)) {
        /*If strDestination or strSource is a NULL pointer[...]these functions return EINVAL */
        res = MKV_ERROR_INVALID_ARGUMENT;
    } else {
        if (*destination != NULL) {
            kvsFree(*destination);
            *destination = NULL;
        }
        if (mallocAndStrcpy_s(destination, source) != 0) {
            res = MKV_ERROR_OUT_OF_MEMORY;
        }
    }

    return res;
}

static int prvBufMallocAndCopy(uint8_t **ppDst, size_t *puDstLen, uint8_t *pSrc, size_t uSrcLen)
{
    int res = MKV_ERRNO_NONE;
    uint8_t *pDst = NULL;
    size_t uDstLen = 0;

    if (ppDst == NULL || puDstLen == NULL || pSrc == NULL || uSrcLen == 0) {
        res = MKV_ERROR_INVALID_ARGUMENT;
    } else if ((pDst = (uint8_t *)kvsMalloc(uSrcLen)) == NULL) {
        res = MKV_ERROR_OUT_OF_MEMORY;
    } else {
        memcpy(pDst, pSrc, uSrcLen);
        *ppDst = pDst;
        *puDstLen = uSrcLen;
    }

    if (res != MKV_ERRNO_NONE) {
        if (pDst != NULL) {
            kvsFree(pDst);
        }
    }

    return res;
}

static void prvStreamFlush(mkv_app_t *pMkv)
{
    StreamHandle xStreamHandle = pMkv->xStreamHandle;
    DataFrameHandle xDataFrameHandle = NULL;
    DataFrameIn_t *pDataFrameIn = NULL;

    while ((xDataFrameHandle = Kvs_streamPop(xStreamHandle)) != NULL) {
        pDataFrameIn = (DataFrameIn_t *)xDataFrameHandle;
        prvCallOnDataFrameTerminate(pDataFrameIn);
        if (pDataFrameIn->pUserData != NULL) {
            kvsFree(pDataFrameIn->pUserData);
        }
        Kvs_dataFrameTerminate(xDataFrameHandle);
    }
}

static int prvStreamFlushToNextCluster(mkv_app_t *pMkv)
{
    int res = MKV_ERRNO_NONE;
    StreamHandle xStreamHandle = pMkv->xStreamHandle;
    DataFrameHandle xDataFrameHandle = NULL;
    DataFrameIn_t *pDataFrameIn = NULL;

    while (1) {
        if (xStreamHandle == NULL) {
            res = MKV_ERROR_INVALID_ARGUMENT;
            break;
        } else if ((xDataFrameHandle = Kvs_streamPeek(xStreamHandle)) == NULL) {
            res = MKV_ERROR_STREAM_NO_AVAILABLE_DATA_FRAME;
            break;
        } else {
            pDataFrameIn = (DataFrameIn_t *)xDataFrameHandle;
            if (pDataFrameIn->xClusterType == MKV_CLUSTER) {
                pMkv->uEarliestTimestamp = pDataFrameIn->uTimestampMs;
                break;
            } else {
                xDataFrameHandle = Kvs_streamPop(xStreamHandle);
                pDataFrameIn = (DataFrameIn_t *)xDataFrameHandle;
                prvCallOnDataFrameTerminate(pDataFrameIn);
                if (pDataFrameIn->pUserData != NULL) {
                    kvsFree(pDataFrameIn->pUserData);
                }
                Kvs_dataFrameTerminate(xDataFrameHandle);
            }
        }
    }

    return res;
}

static void prvStreamFlushHeadUntilMem(mkv_app_t *pMkv, size_t uMemLimit)
{
    StreamHandle xStreamHandle = pMkv->xStreamHandle;
    DataFrameHandle xDataFrameHandle = NULL;
    DataFrameIn_t *pDataFrameIn = NULL;
    size_t uMemTotal = 0;

    while (Kvs_streamMemStatTotal(xStreamHandle, &uMemTotal) == 0 && uMemTotal > uMemLimit && (xDataFrameHandle = Kvs_streamPop(xStreamHandle)) != NULL) {
        pDataFrameIn = (DataFrameIn_t *)xDataFrameHandle;
        prvCallOnDataFrameTerminate(pDataFrameIn);
        if (pDataFrameIn->pUserData != NULL) {
            kvsFree(pDataFrameIn->pUserData);
        }
        Kvs_dataFrameTerminate(xDataFrameHandle);
    }
}

static VideoTrackInfo_t *prvCopyVideoTrackInfo(VideoTrackInfo_t *pSrcVideoTrackInfo)
{
    int res = MKV_ERRNO_NONE;
    VideoTrackInfo_t *pDstVideoTrackInfo = NULL;

    if ((pDstVideoTrackInfo = (VideoTrackInfo_t *)kvsMalloc(sizeof(VideoTrackInfo_t))) == NULL) {
        res = MKV_ERROR_OUT_OF_MEMORY;
    } else {
        memset(pDstVideoTrackInfo, 0, sizeof(VideoTrackInfo_t));

        if ((res = prvMallocAndStrcpyHelper(&(pDstVideoTrackInfo->pTrackName), pSrcVideoTrackInfo->pTrackName)) != MKV_ERRNO_NONE ||
                (res = prvMallocAndStrcpyHelper(&(pDstVideoTrackInfo->pCodecName), pSrcVideoTrackInfo->pCodecName)) != MKV_ERRNO_NONE) {
            /* Propagate the res error */
        } else if ((pDstVideoTrackInfo->pCodecPrivate = (uint8_t *)kvsMalloc(pSrcVideoTrackInfo->uCodecPrivateLen)) == NULL) {
            res = MKV_ERROR_OUT_OF_MEMORY;
        } else {
            pDstVideoTrackInfo->uWidth = pSrcVideoTrackInfo->uWidth;
            pDstVideoTrackInfo->uHeight = pSrcVideoTrackInfo->uHeight;

            memcpy(pDstVideoTrackInfo->pCodecPrivate, pSrcVideoTrackInfo->pCodecPrivate, pSrcVideoTrackInfo->uCodecPrivateLen);
            pDstVideoTrackInfo->uCodecPrivateLen = pSrcVideoTrackInfo->uCodecPrivateLen;
        }
    }

    if (res != MKV_ERRNO_NONE) {
        prvVideoTrackInfoTerminate(pDstVideoTrackInfo);
        pDstVideoTrackInfo = NULL;
    }

    return pDstVideoTrackInfo;
}

static AudioTrackInfo_t *prvCopyAudioTrackInfo(AudioTrackInfo_t *pSrcAudioTrackInfo)
{
    int res = MKV_ERRNO_NONE;
    AudioTrackInfo_t *pDstAudioTrackInfo = NULL;

    if ((pDstAudioTrackInfo = (AudioTrackInfo_t *)kvsMalloc(sizeof(AudioTrackInfo_t))) == NULL) {
        res = MKV_ERROR_OUT_OF_MEMORY;
    } else {
        memset(pDstAudioTrackInfo, 0, sizeof(AudioTrackInfo_t));

        if ((res = prvMallocAndStrcpyHelper(&(pDstAudioTrackInfo->pTrackName), pSrcAudioTrackInfo->pTrackName) != MKV_ERRNO_NONE) ||
                (res = prvMallocAndStrcpyHelper(&(pDstAudioTrackInfo->pCodecName), pSrcAudioTrackInfo->pCodecName)) != MKV_ERRNO_NONE) {
            /* Propagate the res error */
        } else if ((pDstAudioTrackInfo->pCodecPrivate = (uint8_t *)kvsMalloc(pSrcAudioTrackInfo->uCodecPrivateLen)) == NULL) {
            res = MKV_ERROR_OUT_OF_MEMORY;
        } else {
            pDstAudioTrackInfo->uFrequency = pSrcAudioTrackInfo->uFrequency;
            pDstAudioTrackInfo->uChannelNumber = pSrcAudioTrackInfo->uChannelNumber;
            pDstAudioTrackInfo->uBitsPerSample = pSrcAudioTrackInfo->uBitsPerSample;

            memcpy(pDstAudioTrackInfo->pCodecPrivate, pSrcAudioTrackInfo->pCodecPrivate, pSrcAudioTrackInfo->uCodecPrivateLen);
            pDstAudioTrackInfo->uCodecPrivateLen = pSrcAudioTrackInfo->uCodecPrivateLen;
        }
    }

    if (res != MKV_ERRNO_NONE) {
        prvAudioTrackInfoTerminate(pDstAudioTrackInfo);
        pDstAudioTrackInfo = NULL;
    }

    return pDstAudioTrackInfo;
}

static int updateEbmlHeader(mkv_app_t *pMkv)
{
    int res = MKV_ERRNO_NONE;
    uint8_t *pEbmlSeg = NULL;
    size_t uEbmlSegLen = 0;

    if (!(pMkv->isEbmlHeaderUpdated)) {
        // ESP_LOGI(TAG, "Flush to next cluster");
        if ((res = prvStreamFlushToNextCluster(pMkv)) != MKV_ERRNO_NONE) {
            // ESP_LOGI(TAG, "No cluster frame is found");
            /* Propagate the res error */
        } else if ((res = Kvs_streamGetMkvEbmlSegHdr(pMkv->xStreamHandle, &pEbmlSeg, &uEbmlSegLen)) != MKV_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed to update EBML header");
            /* Propagate the res error */
        } else {
            pMkv->isEbmlHeaderUpdated = true;

            for (int i = 0; i < pMkv->onMkvGeneratedCallbacks; i++) {
                /* FIXME: Handle the return value in a proper way. */
                mkv_buf_info_t mkv_buf_info = {
                    .buffer = pEbmlSeg,
                    .type = SEGMENT_HEADER,
                    .uDataLen = uEbmlSegLen,
                    .uTimestampMs = 0
                };
                pMkv->onMkvGeneratedCallbackInfos[i].onMkvGeneratedCallback(pMkv, mkv_buf_info, pMkv->onMkvGeneratedCallbackInfos[i].pAppData);
            }
        }
    }

    return res;
}

static int createStream(mkv_app_t *pMkv)
{
    int res = MKV_ERRNO_NONE;
    VideoTrackInfo_t xVideoTrackInfo = {0};
    uint8_t *pCodecPrivateData = NULL;
    size_t uCodecPrivateDataLen = 0;

    if (pMkv->xStreamHandle == NULL) {
        if (pMkv->pVideoTrackInfo == NULL && pMkv->pSps != NULL && pMkv->pPps != NULL) {
            /* We don't have video track info, but we have SPS & PPS to generate video track info from it. */
            if ((res = NALU_getH264VideoResolutionFromSps(pMkv->pSps, pMkv->uSpsLen, &(xVideoTrackInfo.uWidth), &(xVideoTrackInfo.uHeight))) != MKV_ERRNO_NONE ||
                    (res = Mkv_generateH264CodecPrivateDataFromSpsPps(pMkv->pSps, pMkv->uSpsLen, pMkv->pPps, pMkv->uPpsLen, &pCodecPrivateData, &uCodecPrivateDataLen)) != MKV_ERRNO_NONE) {
                ESP_LOGE(TAG, "Failed to generate video track info");
                /* Propagate the res error */
            } else {
                xVideoTrackInfo.pTrackName = VIDEO_TRACK_NAME;
                xVideoTrackInfo.pCodecName = VIDEO_CODEC_NAME;
                xVideoTrackInfo.pCodecPrivate = pCodecPrivateData;
                xVideoTrackInfo.uCodecPrivateLen = uCodecPrivateDataLen;
                pMkv->pVideoTrackInfo = prvCopyVideoTrackInfo(&xVideoTrackInfo);
            }

            if (pCodecPrivateData != NULL) {
                kvsFree(pCodecPrivateData);
            }
        }

        if (pMkv->pVideoTrackInfo != NULL) {
            if ((pMkv->xStreamHandle = Kvs_streamCreate(pMkv->pVideoTrackInfo, pMkv->pAudioTrackInfo)) == NULL) {
                res = MKV_ERROR_FAIL_TO_CREATE_STREAM_HANDLE;
            } else {
                ESP_LOGI(TAG, "MKV stream buffer created");

                pMkv->isAudioTrackPresent = (pMkv->pAudioTrackInfo != NULL);
            }
        }
    }

    return res;
}

static int checkAndBuildStream(mkv_app_t *pMkv, uint8_t *pData, size_t uDataLen, TrackType_t xTrackType, uint64_t timestamp)
{
    int res = MKV_ERRNO_NONE;
    uint8_t *pSps = NULL;
    size_t uSpsLen = 0;
    uint8_t *pPps = NULL;
    size_t uPpsLen = 0;

    if (pMkv->xStreamHandle == NULL) {
        /* Try to build video track info from frames. */
        if (pMkv->pVideoTrackInfo == NULL && xTrackType == TRACK_VIDEO) {
            if (pMkv->pSps == NULL && NALU_getNaluFromAvccNalus(pData, uDataLen, NALU_TYPE_SPS, &pSps, &uSpsLen) == MKV_ERRNO_NONE) {
                ESP_LOGI(TAG, "SPS is found");
                if ((res = prvBufMallocAndCopy(&(pMkv->pSps), &(pMkv->uSpsLen), pSps, uSpsLen)) != MKV_ERRNO_NONE) {
                    /* Propagate the res error */
                } else {
                    ESP_LOGI(TAG, "SPS is set");
                }
            }
            if (pMkv->pPps == NULL && NALU_getNaluFromAvccNalus(pData, uDataLen, NALU_TYPE_PPS, &pPps, &uPpsLen) == MKV_ERRNO_NONE) {
                ESP_LOGI(TAG, "PPS is found");
                if ((res = prvBufMallocAndCopy(&(pMkv->pPps), &(pMkv->uPpsLen), pPps, uPpsLen)) != MKV_ERRNO_NONE) {
                    /* Propagate the res error */
                } else {
                    ESP_LOGI(TAG, "PPS is set");
                }
            }
        }

        if (pMkv->pSps != NULL && pMkv->pPps != NULL) {
            res = createStream(pMkv);
            if (res == MKV_ERRNO_NONE) {
                pMkv->stream_start_time = timestamp;
            }
        }
    }

    return res;
}

static int prvCheckOnDataFrameToBeSent(DataFrameHandle xDataFrameHandle)
{
    int res = MKV_ERRNO_NONE;
    int retval = 0;
    DataFrameIn_t *pDataFrameIn = NULL;
    data_frame_user_data_t *pUserData = NULL;
    on_data_frame_to_be_sent_t *pOnDataFrameToBeSentCallbackInfo = NULL;

    if (xDataFrameHandle == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
    } else {
        pDataFrameIn = (DataFrameIn_t *)xDataFrameHandle;
        pUserData = pDataFrameIn->pUserData;
        pOnDataFrameToBeSentCallbackInfo = &(pUserData->xCallbacks.onDataFrameToBeSentInfo);
        if (pOnDataFrameToBeSentCallbackInfo->onDataFrameToBeSent != NULL) {
            retval = pOnDataFrameToBeSentCallbackInfo->onDataFrameToBeSent((uint8_t *)(pDataFrameIn->pData), pDataFrameIn->uDataLen, pDataFrameIn->uTimestampMs, pDataFrameIn->xTrackType, pOnDataFrameToBeSentCallbackInfo->pAppData);
            if (retval != 0) {
                res = MKV_GENERATE_CALLBACK_ERROR(retval);
            }
        }
    }

    return res;
}

static int prvGenerateMkvData(mkv_app_t *pMkv, int *pxSendCnt, bool bForceSend)
{
    int res = MKV_ERRNO_NONE;
    int retVal = 0;
    DataFrameHandle xDataFrameHandle = NULL;
    DataFrameIn_t *pDataFrameIn = NULL;
    uint8_t *pData = NULL;
    size_t uDataLen = 0;
    uint8_t *pMkvHeader = NULL;
    size_t uMkvHeaderLen = 0;
    int xSendCnt = 0;

    if (pMkv->xStreamHandle != NULL &&
            pMkv->isEbmlHeaderUpdated == true &&
            Kvs_streamAvailOnTrack(pMkv->xStreamHandle, TRACK_VIDEO) &&
            (!bForceSend || !pMkv->isAudioTrackPresent || Kvs_streamAvailOnTrack(pMkv->xStreamHandle, TRACK_AUDIO))) {
        if ((xDataFrameHandle = Kvs_streamPop(pMkv->xStreamHandle)) == NULL) {
            res = MKV_ERROR_STREAM_NO_AVAILABLE_DATA_FRAME;
            ESP_LOGE(TAG, "Failed to get data frame");
        } else if ((res = prvCheckOnDataFrameToBeSent(xDataFrameHandle)) != MKV_ERRNO_NONE) {
            ESP_LOGI(TAG, "Failed to check OnDataFrameToBeSent");
            /* Propagate the res error */
        } else if ((res = Kvs_dataFrameGetContent(xDataFrameHandle, &pMkvHeader, &uMkvHeaderLen, &pData, &uDataLen)) != MKV_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed to get data and mkv header to send");
            /* Propagate the res error */
        } else if (
            (((DataFrameIn_t *)xDataFrameHandle)->xClusterType == MKV_CLUSTER) && pMkv->tagsListLen > 0 &&
            (res = Kvs_dataFrameAddTags(xDataFrameHandle, pMkv->tagsList, pMkv->tagsListLen, false, &pMkvHeader, &uMkvHeaderLen, &pData, &uDataLen)) != MKV_ERRNO_NONE) {
            ESP_LOGE(TAG, "Failed to add tags");
            /* Propagate the res error */
        } else {
            pDataFrameIn = (DataFrameIn_t *)xDataFrameHandle;
            pMkv->uEarliestTimestamp = pDataFrameIn->uTimestampMs;

            xSendCnt++;

            for (int i = 0; i < pMkv->onMkvGeneratedCallbacks; i++) {
                mkv_buf_info_t header_buf_info = {
                    .buffer = pMkvHeader,
                    .uDataLen = uMkvHeaderLen,
                    .uTimestampMs = pDataFrameIn->uTimestampMs
                };
                mkv_buf_info_t data_buf_info = {
                    .buffer = pData,
                    .uDataLen = uDataLen,
                    .uTimestampMs = pDataFrameIn->uTimestampMs
                };

                if (pDataFrameIn->xClusterType == MKV_CLUSTER) {
                    header_buf_info.type = CLUSTER_HEADER;
                    data_buf_info.type = CLUSTER_BLOCK;
                } else {
                    header_buf_info.type = BLOCK_HEADER;
                    data_buf_info.type = SIMPLE_BLOCK;
                }

                /* FIXME: Handle the return value in a proper way. */
                if ((retVal = pMkv->onMkvGeneratedCallbackInfos[i].onMkvGeneratedCallback(pMkv, header_buf_info, pMkv->onMkvGeneratedCallbackInfos[i].pAppData)) != 0) {
                    res = MKV_GENERATE_CALLBACK_ERROR(retVal);
                } else if ((retVal = pMkv->onMkvGeneratedCallbackInfos[i].onMkvGeneratedCallback(pMkv, data_buf_info, pMkv->onMkvGeneratedCallbackInfos[i].pAppData)) != 0) {
                    res = MKV_GENERATE_CALLBACK_ERROR(retVal);
                } else {
                    /* nop */
                }
            }
        }

        if (xDataFrameHandle != NULL) {
            pDataFrameIn = (DataFrameIn_t *)xDataFrameHandle;
            prvCallOnDataFrameTerminate(pDataFrameIn);
            if (pDataFrameIn->pUserData != NULL) {
                kvsFree(pDataFrameIn->pUserData);
            }
            Kvs_dataFrameTerminate(xDataFrameHandle);
        }
    }

    if (pxSendCnt != NULL) {
        *pxSendCnt = xSendCnt;
    }

    return res;
}

static int prvMkvDoWorkDefault(mkv_app_t *pMkv)
{
    int res = MKV_ERRNO_NONE;
    int xSendCnt = 0;

    do {
        if ((res = updateEbmlHeader(pMkv)) != MKV_ERRNO_NONE) {
            /* Propagate the res error */
            // ESP_LOGE(TAG, "updateEbmlHeader res: %d", res);
            //
            break;
        }

        if ((res = prvGenerateMkvData(pMkv, &xSendCnt, false)) != MKV_ERRNO_NONE) {
            /* Propagate the res error */
            ESP_LOGE(TAG, "prvGenerateMkvData res: %d", res);
            break;
        }
    } while (false);

    if (xSendCnt == 0) {
        sleepInMs(50);
    }

    return res;
}

static int setupTagsForSession(mkv_app_t *pMkv)
{
    int res = MKV_ERRNO_NONE;
    const size_t tagsListLen = MAX_TAG_AMOUNT - 1; // Note: end of fragment tag counts towards the 10 limit
    MkvTag_t *tags = kvsMalloc(tagsListLen * sizeof(MkvTag_t));
    if (tags == NULL) {
        res = MKV_ERROR_OUT_OF_MEMORY;
        ESP_LOGE(TAG, "OOM: tagsList");
    } else {
        for (int i = 1; i <= tagsListLen; i++) {
            snprintf((char *)tags[i - 1].key, MAX_TAG_NAME_LEN, "test_key_%d", i);
            snprintf((char *)tags[i - 1].value, MAX_TAG_VALUE_LEN, "test_value_%d", i);
        }

        pMkv->tagsList = tags;
        pMkv->tagsListLen = tagsListLen;

        ESP_LOGI(TAG, "Setup tags for session");
    }

    return res;
}

static void mkv_task(void *pvParameters)
{
    mkv_app_handle mkvAppHandle = (mkv_app_handle) pvParameters;

    uint64_t uLastPrintMemStatTimestamp = 0;

    int res;

    static uint32_t notification = 0;

    while (true) {
        mkvAppHandle->state = TASK_STOPPED;
        ESP_LOGI(TAG, "MKV MUX TASK STOPPED");

        while (!(notification & START_TASK_SIGNAL)) {
            xTaskNotifyWait(
                0,
                UINT32_MAX,
                &notification,
                portMAX_DELAY);
        }

        notification = 0;
        mkvAppHandle->state = TASK_STARTED;
        ESP_LOGI(TAG, "MKV MUX TASK STARTED");

        while (true) {
            if (xTaskNotifyWait(0, UINT32_MAX, &notification, 1) == pdTRUE && (notification & STOP_TASK_SIGNAL)) {
                break;
            }
            if ((res = mkv_app_do_work(mkvAppHandle)) != MKV_ERRNO_NONE) {
                // ESP_LOGE(TAG, "Mkv app do work err: %d", res);
            } else if ((esp_timer_get_time() / 1000) > uLastPrintMemStatTimestamp + 1000) {
                // ESP_LOGI(TAG, "Buffer memory used: %zu", MkvApp_getStreamMemStatTotal(mkvAppHandle));
                uLastPrintMemStatTimestamp = esp_timer_get_time() / 1000;
            }
        }
    }
}

mkv_app_handle mkv_app_init(mkv_app_config_t *config)
{
    int res = MKV_ERRNO_NONE;
    mkv_app_handle pMkv = NULL;
    void *task_stack, *task_buffer;

    if ((pMkv = (mkv_app_t *)kvsMalloc(sizeof(mkv_app_t))) == NULL) {
        res = MKV_ERROR_OUT_OF_MEMORY;
        ESP_LOGE(TAG, "OOM: pMkv");
    } else if (!(task_stack = heap_caps_malloc_prefer(MKV_MUX_TASK_STACK_SIZE, 1, MALLOC_CAP_INTERNAL)) ||
               !(task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL))) {
        res = MKV_ERROR_OUT_OF_MEMORY;
    } else {
        memset(pMkv, 0, sizeof(mkv_app_t));

        if (config->stream_policy >= STREAM_POLICY_MAX || config->ring_buf_mem == 0) {
            ESP_LOGE(TAG, "Invalid MkvApp configuration");
            res = MKV_ERROR_INVALID_ARGUMENT;
        } else {

            pMkv->uEarliestTimestamp = 0;
            pMkv->xStreamHandle = NULL;
            pMkv->isEbmlHeaderUpdated = false;
            pMkv->xStrategy.xPolicy = config->stream_policy;
            pMkv->xStrategy.xRingBufferPara.uMemLimit = config->ring_buf_mem;

            pMkv->pVideoTrackInfo = NULL;
            pMkv->isAudioTrackPresent = false;
            pMkv->pAudioTrackInfo = NULL;
            pMkv->onMkvGeneratedCallbackInfos = NULL;
            pMkv->MkvApp_task_handle = NULL;
            if ((res = setupTagsForSession(pMkv)) != MKV_ERRNO_NONE) {
                ESP_LOGE(TAG, "Failed to setup tags");
                /* Propagate the res error */
            }
        }
    }

    if (res != MKV_ERRNO_NONE) {
        mkv_app_deinit(pMkv);
        pMkv = NULL;
    } else {
        pMkv->state = TASK_INITIATED;
        pMkv->MkvApp_task_handle = xTaskCreateStatic(&mkv_task, "mkv muxer task", MKV_MUX_TASK_STACK_SIZE,
                                                     pMkv, 5, (StackType_t *) task_stack, (StaticTask_t *) task_buffer);
    }

    return (mkv_app_handle)pMkv;
}

mkv_err_t mkv_app_stop(mkv_app_handle handle)
{
    mkv_app_t *pMkv = (mkv_app_t *)handle;
    mkv_err_t err = MKV_ERRNO_NONE;

    if (pMkv == NULL) {
        err = MKV_ERROR_INVALID_ARGUMENT;
    } else if (pMkv->MkvApp_task_handle == NULL) {
        err = MKV_ERROR_INVALID_STATE;
    } else if (pMkv->state == TASK_STOPPED) {
        err = MKV_ERROR_INVALID_STATE;
        ESP_LOGW(TAG, "Mkv app already stopped");
    } else {
        xTaskNotify(pMkv->MkvApp_task_handle,
                    STOP_TASK_SIGNAL,
                    eSetValueWithOverwrite);
        ESP_LOGI(TAG, "Sent stop signal to task");

        while (pMkv->state != TASK_STOPPED) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        ESP_LOGI(TAG, "Task stopped");

        if (pMkv->xStreamHandle != NULL) {
            prvStreamFlush(pMkv);
            Kvs_streamTermintate(pMkv->xStreamHandle);
            pMkv->xStreamHandle = NULL;
        }
    }

    return err;
}

mkv_err_t mkv_app_deinit(mkv_app_handle handle)
{
    mkv_app_t *pMkv = (mkv_app_t *)handle;
    mkv_err_t err = MKV_ERRNO_NONE;

    if (pMkv == NULL) {
        err = MKV_ERROR_INVALID_ARGUMENT;
    } else {
        mkv_app_stop(pMkv);

        if (pMkv->xStreamHandle != NULL) {
            prvStreamFlush(pMkv);
            Kvs_streamTermintate(pMkv->xStreamHandle);
            pMkv->xStreamHandle = NULL;
        }
        if (pMkv->pVideoTrackInfo != NULL) {
            prvVideoTrackInfoTerminate(pMkv->pVideoTrackInfo);
        }
        if (pMkv->pAudioTrackInfo != NULL) {
            prvAudioTrackInfoTerminate(pMkv->pAudioTrackInfo);
        }
        if (pMkv->pSps != NULL) {
            kvsFree(pMkv->pSps);
            pMkv->pSps = NULL;
        }
        if (pMkv->pPps != NULL) {
            kvsFree(pMkv->pPps);
            pMkv->pPps = NULL;
        }
        if (pMkv->tagsList != NULL) {
            kvsFree(pMkv->tagsList);
            pMkv->tagsList = NULL;
        }

        if (pMkv->MkvApp_task_handle) {
            vTaskDelete(pMkv->MkvApp_task_handle);
            pMkv->MkvApp_task_handle = NULL;
        }

        memset(pMkv, 0, sizeof(mkv_app_t));
        kvsFree(pMkv);
    }

    return err;
}

bool mkv_app_is_started(mkv_app_handle handle)
{
    mkv_app_t *pMkv = (mkv_app_t *) handle;
    return pMkv && pMkv->state == TASK_STARTED;
}

mkv_err_t mkv_app_start(mkv_app_handle handle)
{
    int res = MKV_ERRNO_NONE;
    mkv_app_t *pMkv = (mkv_app_t *)handle;

    if (pMkv == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
    } else if (pMkv->state == TASK_STARTED) {
        ESP_LOGW(TAG, "Mkv app already started");
        res = MKV_ERROR_INVALID_STATE;
    } else {
        ESP_LOGI(TAG, "Starting mkv app");
        xTaskNotify(pMkv->MkvApp_task_handle,
                    START_TASK_SIGNAL,
                    eSetValueWithOverwrite);
    }

    return res;
}

mkv_err_t mkv_app_add_frame(mkv_app_handle handle, uint8_t *pData, size_t uDataLen, size_t uDataSize, uint64_t uTimestamp, TrackType_t xTrackType)
{
    return mkv_app_add_frame_with_callbacks(handle, pData, uDataLen, uDataSize, uTimestamp, xTrackType, NULL);
}

mkv_err_t mkv_app_add_frame_with_callbacks(mkv_app_handle handle, uint8_t *pData, size_t uDataLen, size_t uDataSize, uint64_t uTimestamp, TrackType_t xTrackType, data_frame_callbacks_t *pCallbacks)
{
    int res = MKV_ERRNO_NONE;
    int retVal = 0;
    mkv_app_t *pMkv = (mkv_app_t *)handle;
    DataFrameIn_t xDataFrameIn = {0};
    data_frame_user_data_t *pUserData = NULL;

    if (pMkv == NULL || pData == NULL || uDataLen == 0) {
        res = MKV_ERROR_INVALID_ARGUMENT;
    } else if (pMkv->state != TASK_STARTED) {
        res = MKV_ERROR_INVALID_STATE;
    } else if (uTimestamp < pMkv->uEarliestTimestamp) {
        res = MKV_ERROR_ADD_FRAME_WHOSE_TIMESTAMP_GOES_BACK;
    } else if (
        xTrackType == TRACK_VIDEO && NALU_isAnnexBFrame(pData, uDataLen) &&
        (res = NALU_convertAnnexBToAvccInPlace(pData, uDataLen, uDataSize, (uint32_t *)&uDataLen)) != MKV_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to convert Annex-B to Avcc in place");
        /* Propagate the res error */
    } else if ((res = checkAndBuildStream(pMkv, pData, uDataLen, xTrackType, uTimestamp)) != MKV_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to build stream buffer");
        /* Propagate the res error */
    } else if (pMkv->xStreamHandle == NULL) {
        res = MKV_ERROR_STREAM_NOT_READY;
    } else if ((pUserData = (data_frame_user_data_t *)kvsMalloc(sizeof(data_frame_user_data_t))) == NULL) {
        res = MKV_ERROR_OUT_OF_MEMORY;
        ESP_LOGE(TAG, "OOM: pUserData");
    } else {
        xDataFrameIn.pData = (char *)pData;
        xDataFrameIn.uDataLen = uDataLen;
        xDataFrameIn.bIsKeyFrame = (xTrackType == TRACK_VIDEO) ? isKeyFrame(pData, uDataLen) : false;
        xDataFrameIn.uTimestampMs = uTimestamp - pMkv->stream_start_time;
        xDataFrameIn.xTrackType = xTrackType;
        xDataFrameIn.xClusterType = (xDataFrameIn.bIsKeyFrame) ? MKV_CLUSTER : MKV_SIMPLE_BLOCK;

        memset(pUserData, 0, sizeof(data_frame_user_data_t));
        if (pCallbacks == NULL) {
            /* Assign default callbacks. */
            pUserData->xCallbacks.onDataFrameTerminateInfo.onDataFrameTerminate = defaultOnDataFrameTerminate;
            pUserData->xCallbacks.onDataFrameTerminateInfo.pAppData = NULL;
            pUserData->xCallbacks.onDataFrameToBeSentInfo.onDataFrameToBeSent = NULL;
            pUserData->xCallbacks.onDataFrameToBeSentInfo.pAppData = NULL;
        } else {
            memcpy(&(pUserData->xCallbacks), pCallbacks, sizeof(data_frame_callbacks_t));
        }
        xDataFrameIn.pUserData = pUserData;

        if (pMkv->xStrategy.xPolicy == STREAM_POLICY_RING_BUFFER) {
            prvStreamFlushHeadUntilMem(pMkv, pMkv->xStrategy.xRingBufferPara.uMemLimit);
        }

        if (Kvs_streamAddDataFrame(pMkv->xStreamHandle, &xDataFrameIn) == NULL) {
            res = MKV_ERROR_FAIL_TO_ADD_DATA_FRAME_TO_STREAM;
            ESP_LOGE(TAG, "Failed to add data frame");
        }
    }

    if (res != MKV_ERRNO_NONE) {
        if (pData != NULL) {
            if (pCallbacks != NULL && pCallbacks->onDataFrameTerminateInfo.onDataFrameTerminate != NULL) {
                retVal = pCallbacks->onDataFrameTerminateInfo.onDataFrameTerminate(pData, uDataLen, uTimestamp, xTrackType, NULL);
            } else {
                retVal = defaultOnDataFrameTerminate(pData, uDataLen, uTimestamp, xTrackType, NULL);
            }
            if (retVal != 0) {
                res = MKV_GENERATE_CALLBACK_ERROR(retVal);
            }
        }
        if (pUserData != NULL) {
            kvsFree(pUserData);
        }
    }

    return res;
}

mkv_err_t mkv_app_do_work(mkv_app_handle handle)
{
    int res = MKV_ERRNO_NONE;
    mkv_app_t *pMkv = (mkv_app_t *)handle;

    if (pMkv == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
    } else {
        res = prvMkvDoWorkDefault(pMkv);
    }

    return res;
}

size_t mkv_app_get_stream_mem_stat_total(mkv_app_handle handle)
{
    size_t uMemTotal = 0;
    mkv_app_t *pMkv = (mkv_app_t *)handle;

    if (pMkv != NULL && pMkv->xStreamHandle != NULL && Kvs_streamMemStatTotal(pMkv->xStreamHandle, &uMemTotal) == MKV_ERRNO_NONE) {
        return uMemTotal;
    } else {
        return 0;
    }
}

mkv_err_t mkv_app_emit_ebml_header(mkv_app_handle handle, mkv_buf_info_t *mkv_buf)
{
    if (!handle || !handle->isEbmlHeaderUpdated) {
        return MKV_ERROR_INVALID_STATE;
    }

    if (!mkv_buf) {
        return MKV_ERROR_INVALID_ARGUMENT;
    }

    uint8_t *buffer;

    if (Kvs_streamGetMkvEbmlSegHdr(handle->xStreamHandle, &buffer, &mkv_buf->uDataLen) != MKV_ERRNO_NONE) {
        return MKV_ERROR_INVALID_STATE;
    }

    mkv_buf->buffer = heap_caps_malloc(mkv_buf->uDataLen, MALLOC_CAP_SPIRAM);
    if (!(mkv_buf->buffer)) {
        return MKV_ERROR_OUT_OF_MEMORY;
    }

    memcpy(mkv_buf->buffer, buffer, mkv_buf->uDataLen);
    mkv_buf->type = SEGMENT_HEADER;
    mkv_buf->uTimestampMs = 0;

    return MKV_ERRNO_NONE;
}

mkv_err_t mkv_app_set_on_mkv_generated_callbacks(mkv_app_handle handle, on_mkv_generated_callback_info_t *onMkvGeneratedCallbackInfos, uint8_t size)
{
    mkv_err_t res = MKV_ERRNO_NONE;
    mkv_app_t *pMkv = (mkv_app_t *)handle;

    if (pMkv == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
    } else if (size == 0) {
        if (pMkv->onMkvGeneratedCallbackInfos) {
            kvsFree(pMkv->onMkvGeneratedCallbackInfos);
        }
        pMkv->onMkvGeneratedCallbackInfos = NULL;
    } else if (onMkvGeneratedCallbackInfos == NULL) {
        res = MKV_ERROR_INVALID_ARGUMENT;
    } else {
        pMkv->onMkvGeneratedCallbacks = size;
        pMkv->onMkvGeneratedCallbackInfos = kvsMalloc(sizeof(on_mkv_generated_callback_info_t) * pMkv->onMkvGeneratedCallbacks);
        memcpy(pMkv->onMkvGeneratedCallbackInfos, onMkvGeneratedCallbackInfos, sizeof(on_mkv_generated_callback_info_t) * pMkv->onMkvGeneratedCallbacks);
    }

    return res;
}
