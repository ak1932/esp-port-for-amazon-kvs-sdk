/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief The two consumers of the shared capture pipeline, each start/stoppable
 *        on its own.
 *
 * That independence is the whole point of this example. Both sides attach to the
 * same camera and the same H.264 encoder, and both do it the same way: each
 * registers an encoded-video sink (video_sink.h) and receives frames as borrowed
 * buffers on the encoder task.
 *
 * What they do with the frame differs, and that is the interesting part:
 *
 *   - PutMedia copies it into the MKV muxer and returns. That fits inside a
 *     callback.
 *   - WebRTC copies it onto its own queue and returns; the RTP send happens on
 *     its own thread, because writeFrame() packetizes inline and would otherwise
 *     stall capture for PutMedia too.
 *
 * Underneath, video_capture_init/start/stop/deinit are reference counted, so the
 * camera comes up on the first consumer and goes down after the last.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PutMedia: capture -> MKV mux -> KVS PutMedia upload. */
esp_err_t putmedia_start(void);
esp_err_t putmedia_stop(void);
bool      putmedia_is_running(void);

/* WebRTC: KVS signaling + peer connections, streaming the same encoded video. */
esp_err_t webrtc_side_start(void);
esp_err_t webrtc_side_stop(void);
bool      webrtc_side_is_running(void);

#ifdef __cplusplus
}
#endif
