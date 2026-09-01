/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/*
 * Camera-side encoder controls, for the UVC H.264 passthrough path.
 *
 * These are NOT esp_video API. They are added to the managed component by
 * patches/managed_components/0003, which in turn relies on the UVC H.264 extension
 * unit support added by 0002, and esp_video publishes no header for them - so the
 * prototypes live here rather than being implicitly declared at each call site (which
 * is what the passthrough path did before, and it did not compile).
 */

/**
 * @brief Ask the camera to emit a key frame now
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE if no stream is running, or an error from the
 *         camera - cameras that do not implement the control STALL EP0, so a caller
 *         should ask once and remember the answer rather than retrying.
 */
esp_err_t esp_video_uvc_request_key_frame(void);

/**
 * @brief Bitrate the camera committed at stream start
 *
 * This, not the configured request, is the ceiling a rate controller may work down
 * from: the camera clamps freely.
 *
 * @param[out] bps Receives the committed bitrate
 * @return ESP_OK; ESP_ERR_NOT_SUPPORTED when this camera has no usable H.264 extension
 *         unit, which is also the answer to "can anything be throttled here at all";
 *         ESP_ERR_INVALID_STATE when no stream is running
 */
esp_err_t esp_video_uvc_get_h264_committed_bitrate(uint32_t *bps);

/**
 * @brief Change the camera's H.264 bitrate mid-stream
 *
 * Leaves the key-frame period alone, so the short period committed at stream start
 * survives.
 *
 * @param[in]  bitrate_bps   Requested bitrate
 * @param[out] committed_bps If non-NULL, what the camera agreed to. A report, not a
 *                           guarantee - a camera whose pipeline has died accepts these
 *                           writes silently.
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED, or ESP_ERR_INVALID_STATE as above
 */
esp_err_t esp_video_uvc_set_h264_bitrate(uint32_t bitrate_bps, uint32_t *committed_bps);

/**
 * @brief Choose what gives way when the bitrate is too low: frame rate or quality
 *
 * Added by patches/managed_components/0002 (the hint) and 0003 (this setter). Records
 * a value and performs no I/O; it is re-asserted at stream start and on every
 * mid-stream bitrate change, so the choice survives both actuation paths.
 *
 * @param interval_100ns Frame interval to pin, in 100 ns units - so 333333 for 30 fps.
 *                       0 leaves the frame rate at the camera's discretion, which is
 *                       what makes it hold quality and shed frame rate instead
 *                       (measured: 1080p at 256 kbps -> 4 fps).
 * @return ESP_OK
 */
esp_err_t esp_video_uvc_set_h264_frame_interval(uint32_t interval_100ns);

/* One second in the 100 ns units UVC expresses frame intervals in. */
#define MEDIA_STREAM_UVC_INTERVAL_UNITS_PER_S  10000000u

/* Reference the bitrate budget is expressed at, mirroring UVCX_H264_BITRATE_REF_* in
 * patches/managed_components/0002. Duplicated rather than exported because it lives in
 * usb_host_uvc, two components away; if 0002 ever changes its reference, this must
 * change with it or every converted value is silently wrong. */
#define MEDIA_STREAM_UVC_BITRATE_REF_W       1280u
#define MEDIA_STREAM_UVC_BITRATE_REF_H       720u
#define MEDIA_STREAM_UVC_BITRATE_REF_PIXELS  (MEDIA_STREAM_UVC_BITRATE_REF_W * MEDIA_STREAM_UVC_BITRATE_REF_H)
