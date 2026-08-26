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

#ifndef MKVAPP_H
#define MKVAPP_H

#include "kvs/mkv_errors.h"
#include "kvs/mkv_generator.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    SEGMENT_HEADER,
    CLUSTER_HEADER,
    BLOCK_HEADER,
    CLUSTER_BLOCK,
    SIMPLE_BLOCK
} mkv_buf_type_t;

typedef struct {
    uint8_t *buffer;
    mkv_buf_type_t type;
    uint64_t uTimestampMs;
    size_t uDataLen;
} mkv_buf_info_t;

typedef struct mkv_app *mkv_app_handle;

/**
 * This callback is called when a data frame is about to be terminated. And this callback is responsible to return the control to application side.
 * The application can either just called free() or handle it in a proper way which depends on the way how this frame is created.
 *
 * @param[in] pData Pointer of data frame
 * @param[in] uDataLen Size of data frame
 * @param[in] uTimestamp Timestamp of data frame
 * @param[in] xTrackType Track type of data frame
 * @param[in] pAppData Pointer of application data that is assigned in function MkvApp_addFrameWithCallbacks()
 * @return 0 on success, non-zero value otherwise
 */
typedef mkv_err_t (*on_data_frame_terminate_callback_t)(uint8_t *pData, size_t uDataLen, uint64_t uTimestamp, TrackType_t xTrackType, void *pAppData);

/**
 * This callback is called before a frame is about to be sent. Application can use this callback to decide whether or not to send out this frame.
 *
 * @param[in] pData Pointer of data frame
 * @param[in] uDataLen Size of data frame
 * @param[in] uTimestamp Timestamp of data frame
 * @param[in] xTrackType Track type of data frame
 * @param[in] pAppData Pointer of application data that is assigned in function MkvApp_addFrameWithCallbacks()
 */
typedef mkv_err_t (*on_data_frame_to_be_sent_callback_t)(uint8_t *pData, size_t uDataLen, uint64_t uTimestamp, TrackType_t xTrackType, void *pAppData);

/**
 * This callback is called whenever mkv is generated
 *
 * @param[in] handle MkvAppHandle which is generating the callback
 * @param[in] mkv_buf_info Descriptor of mkv which is generated
 * @param[in] pAppData Pointer of application data that is assigned in function MkvApp_setOnMkvGeneratedCallback()
 */
typedef mkv_err_t (*on_mkv_generated_callback_t)(mkv_app_handle handle, mkv_buf_info_t mkv_buf_info, void *pAppData);

typedef struct on_mkv_generated_callback_info {
    on_mkv_generated_callback_t onMkvGeneratedCallback;
    void *pAppData;
} on_mkv_generated_callback_info_t;

typedef struct on_data_frame_terminate_info {
    on_data_frame_terminate_callback_t onDataFrameTerminate;
    void *pAppData;
} on_data_frame_terminate_info_t;

typedef struct on_data_frame_to_be_sent_callback_info_t {
    on_data_frame_to_be_sent_callback_t onDataFrameToBeSent;
    void *pAppData;
} on_data_frame_to_be_sent_t;

typedef struct data_frame_callbacks {
    on_data_frame_terminate_info_t onDataFrameTerminateInfo;
    on_data_frame_to_be_sent_t onDataFrameToBeSentInfo;
} data_frame_callbacks_t;

typedef enum mkv_app_stream_policy {
    STREAM_POLICY_NONE = 0,
    STREAM_POLICY_RING_BUFFER,
    STREAM_POLICY_MAX
} mkv_app_stream_policy_t;

typedef struct {
    mkv_app_stream_policy_t stream_policy;
    ssize_t               ring_buf_mem;
} mkv_app_config_t;

/**
 * Initiate a MKV application.
 *
 * @return MKV application handle
 */
mkv_app_handle mkv_app_init(mkv_app_config_t *config);

/**
 * Starts MKV application. It includes validating if stream exist,
 * It also tries to setup stream buffer if track info are already set.
 *
 * @param[in] handle MKV application handle.
 * @return 0 on success, non-zero value otherwise
 */
mkv_err_t mkv_app_start(mkv_app_handle handle);

/**
 * Checks if mkv app is already started.
 *
 * @param[in] handle MKV application handle.
 * @return true if initialized and started, false otherwise
 */
bool mkv_app_is_started(mkv_app_handle handle);

/**
 * Stop MKV application. It flushes stream buffer and terminates it.
 *
 * @param[in] handle MKV application handle
 * @return 0 on success, non-zero value otherwise
 */
mkv_err_t mkv_app_stop(mkv_app_handle handle);

/**
 * Deinitiate MKV application. It flushes stream buffer and terminate it.
 * You must initiate mkv app before starting it again.
 *
 * @param[in] handle MKV application handle
 * @return 0 on success, non-zero value otherwise
 */
mkv_err_t mkv_app_deinit(mkv_app_handle handle);

/**
 * Add a frame to MKV application. If the stream buffer is not allocated yet, then it'll try to parse decode information
 * and then setup stream buffer.
 *
 * @param[in] handle MKV application handle
 * @param[in] pData Data buffer pointer
 * @param[in] uDataLen Data length
 * @param[in] uDataSize Data buffer size
 * @param[in] uTimestamp Frame absolution timestamp in milliseconds.
 * @param[in] xTrackType Track type, it could be TRACK_VIDEO or TRACK_AUDIO
 * @return 0 on success, non-zero value otherwise
 */
mkv_err_t mkv_app_add_frame(mkv_app_handle handle, uint8_t *pData, size_t uDataLen, size_t uDataSize, uint64_t uTimestamp, TrackType_t xTrackType);

/**
 * Add a frame to MKV application. If the stream buffer is not allocated yet, then it'll try to parse decode information
 * and then setup stream buffer.
 *
 * @param[in] handle MKV application handle
 * @param[in] pData Data buffer pointer
 * @param[in] uDataLen Data length
 * @param[in] uDataSize Data buffer size
 * @param[in] uTimestamp Frame absolution timestamp in milliseconds.
 * @param[in] xTrackType Track type, it could be TRACK_VIDEO or TRACK_AUDIO
 * @param[in] pCallbacks Callbacks
 * @return 0 on success, non-zero value otherwise
 */
mkv_err_t mkv_app_add_frame_with_callbacks(mkv_app_handle handle, uint8_t *pData, size_t uDataLen, size_t uDataSize, uint64_t uTimestamp, TrackType_t xTrackType, data_frame_callbacks_t *pCallbacks);

/**
 * Get the memory used in the stream buffer.
 *
 * @param handle  MKV application handle
 * @return Memory used in current stream buffer, zero value otherwise
 */
size_t mkv_app_get_stream_mem_stat_total(mkv_app_handle handle);

/**
 * Set onMkvGeneratedCallbacks. Whenever a part of mkv file is generated, it'll invoke this callback.
 *
 * @param handle MKV application handle
 * @param onMkvGeneratedCallbackInfos Callback array
 * @param size The size of the onMkvGeneratedCallbacks array
 * @return 0 on success, non-zero value otherwise
 */
mkv_err_t mkv_app_set_on_mkv_generated_callbacks(mkv_app_handle handle, on_mkv_generated_callback_info_t *on_mkv_generated_callback_infos, uint8_t size);

/**
 * Emit Ebml Header for current mkv app stream.
 *
 * @param[in] handle MKV application handle
 * @param[in] mkv_buf Pointer to mkv buffer descriptor
 * @return MKV_ERRNO_NONE on success
 *         MKV_ERROR_* on failure
 */
mkv_err_t mkv_app_emit_ebml_header(mkv_app_handle handle, mkv_buf_info_t *mkv_buf);

#endif /* MKVAPP_H */
