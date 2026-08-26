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

#pragma once

typedef enum MkvModule {
    MKV_MODULE_COMMON = 0,
    MKV_MODULE_CALLBACK = 1,
} MkvModule_t;

#define MKV_ERROR_COMMON_BASE           (MKV_MODULE_COMMON<<16)
#define MKV_ERROR_CALLBACK_BASE         (MKV_MODULE_CALLBACK<<16)

#define MKV_GENERATE_COMMON_ERROR(xErrorCode)           (-(MKV_ERROR_COMMON_BASE | (xErrorCode)))
#define MKV_GENERATE_CALLBACK_ERROR(xErrorCode)         (-(MKV_ERROR_CALLBACK_BASE | -(xErrorCode)))

/* Generic errors */
typedef enum {
    MKV_ERROR_GENERIC                               = (-(MKV_ERROR_COMMON_BASE + 0x0001)),
    MKV_ERROR_INVALID_ARGUMENT                      = (-(MKV_ERROR_COMMON_BASE + 0x0002)),
    MKV_ERROR_OUT_OF_MEMORY                         = (-(MKV_ERROR_COMMON_BASE + 0x0003)),
    MKV_ERROR_LOCK_ERROR                            = (-(MKV_ERROR_COMMON_BASE + 0x0004)),
    MKV_ERROR_C_UTIL_STRING_ERROR                   = (-(MKV_ERROR_COMMON_BASE + 0x0005)),
    MKV_ERROR_C_UTIL_UNABLE_TO_CREATE_BUFFER        = (-(MKV_ERROR_COMMON_BASE + 0x0006)),
    MKV_ERROR_C_UTIL_UNABLE_TO_ENLARGE_BUFFER       = (-(MKV_ERROR_COMMON_BASE + 0x0007)),
    MKV_ERROR_TLSF_FAILED_TO_CREATE_POOL            = (-(MKV_ERROR_COMMON_BASE + 0x0008)),
    MKV_ERROR_INVALID_STATE                         = (-(MKV_ERROR_COMMON_BASE + 0x0009)),
    MKV_ERROR_TIMEOUT                               = (-(MKV_ERROR_COMMON_BASE + 0x0009)),

    /* PUT_MEDIA errors */
    MKV_ERROR_MKV_UNKNOWN_CLUSTER_TYPE        = (-(MKV_ERROR_COMMON_BASE + 0x0201)),
    MKV_ERROR_AVCC_NALU_IS_BROKEN                   = (-(MKV_ERROR_COMMON_BASE + 0x0202)),
    MKV_ERROR_NALU_TYPE_NOT_FOUND                   = (-(MKV_ERROR_COMMON_BASE + 0x0203)),
    MKV_ERROR_INVALID_NALU_FORMAT                   = (-(MKV_ERROR_COMMON_BASE + 0x0204)),
    MKV_ERROR_MISSING_NALU                          = (-(MKV_ERROR_COMMON_BASE + 0x0205)),
    MKV_ERROR_EXCEED_MAX_NALU_COUNT_LIMIT           = (-(MKV_ERROR_COMMON_BASE + 0x0206)),
    MKV_ERROR_NO_ENOUGH_SPACE_FOR_NALU_CONVERSION   = (-(MKV_ERROR_COMMON_BASE + 0x0207)),
    MKV_ERROR_MKV_INVALID_AUDIO_FREQUENCY     = (-(MKV_ERROR_COMMON_BASE + 0x0208)),

    /* Streaming errors */
    MKV_ERROR_STREAM_MKV_IS_NOT_INITIALIZED   = (-(MKV_ERROR_COMMON_BASE + 0x0301)),
    MKV_ERROR_INVALID_CLUSTER_HDR_LEN               = (-(MKV_ERROR_COMMON_BASE + 0x0302)),
    MKV_ERROR_STREAM_NO_AVAILABLE_DATA_FRAME        = (-(MKV_ERROR_COMMON_BASE + 0x0303)),
    MKV_ERROR_FAIL_TO_CREATE_STREAM_HANDLE          = (-(MKV_ERROR_COMMON_BASE + 0x0304)),
    MKV_ERROR_INVALID_STREAM_POLICY                 = (-(MKV_ERROR_COMMON_BASE + 0x0305)),
    MKV_ERROR_ADD_FRAME_WHOSE_TIMESTAMP_GOES_BACK   = (-(MKV_ERROR_COMMON_BASE + 0x0306)),
    MKV_ERROR_STREAM_NOT_READY                      = (-(MKV_ERROR_COMMON_BASE + 0x0307)),
    MKV_ERROR_FAIL_TO_ADD_DATA_FRAME_TO_STREAM      = (-(MKV_ERROR_COMMON_BASE + 0x0308)),

    /* PUT_MEDIA application errors */
    MKV_ERROR_PUT_MEDIAAPP_UNKNOWN_DO_WORK_TYPE     = (-(MKV_ERROR_COMMON_BASE + 0x0341)),
    MKV_ERROR_PAST_OLD_TIME                         = (-(MKV_ERROR_COMMON_BASE + 0x0342)),

    MKV_ERRNO_NONE                                  = 0,
    MKV_ERRNO_FAIL                                  = MKV_ERROR_GENERIC,
} mkv_err_t;
