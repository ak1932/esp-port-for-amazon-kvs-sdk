/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * (OPT IN) Congestion-adaptive TX-video rate control, one controller per sink.
 *
 * There is exactly one H.264 encoder, and an encoded frame cannot be dropped for
 * one consumer and kept for another - every P-frame references the ones before
 * it, and the hardware encoder has no temporal layers to drop cleanly. So every
 * sink necessarily receives every encoded frame.
 *
 * "Per sink" therefore means a controller per sink, not a stream per sink. Each
 * transport runs its own ladder position, its own congestion signal and its own
 * enable state; the grabber then asks this module for ONE operating point, which
 * it arbitrates as the strict minimum over every eligible controller.
 *
 * Worst sink wins. That is deliberate: the encoder cannot serve a fast consumer
 * and a slow one at once.
 *
 * Strategy inside a controller: fps and bitrate are tied to a single operating-
 * point LADDER (see video_rate_ctrl.c) rather than tracked independently - each
 * rung is a (fps, bitrate) pair chosen to make sense together.
 *
 * Which of the two columns actually actuates depends on who owns the encoder.
 * Lowering fps means dropping a frame BEFORE encode, so the encoder's reference
 * chain never sees a gap. On UVC H.264 passthrough there is no such moment - the
 * camera delivers finished frames - so fps is not actuable there at all and only
 * the bitrate column moves, through the camera's own H.264 extension unit. That
 * is what video_rate_ctrl_init()'s fps_actuable argument selects.
 *
 * Two congestion front-ends can feed that one ladder
 *
 *   - video_rate_ctrl_report_send_ms()  - a RATE signal. How long one frame took
 *     to hand off. Used by the WebRTC path.
 *
 *   - video_rate_ctrl_report_queue()    - a LEVEL signal. How full a store-and-
 *     forward upload queue is. Used by the PutMedia path.
 *
 * A network-side ceiling (WebRTC TWCC sender BWE) caps how far one controller may
 * recover; see video_rate_ctrl_set_network_ceiling_bps().
 *
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to one sink's rate controller
 */
typedef struct video_rate_ctrl_s *video_rate_ctrl_handle_t;

/**
 * @brief Declare the encoder the controllers are steering
 *
 * Idempotent, and independent of controller lifetime: capture is reference
 * counted, so this runs on the first camera bring-up and again on a restart,
 * whereas a transport may have created and enabled its controller long before.
 * Any controller already enabled is re-seeded to the ladder's starting rung.
 *
 * @param camera_fps      Native capture rate; skip decisions are relative to this
 * @param max_bitrate_bps Configured encoder ceiling (ladder L0 = 100% of this);
 *                        no controller ever asks for more
 * @param width           Encoded width, for logs and diagnostics
 * @param height          Encoded height, for logs and diagnostics
 * @param fps_actuable    Whether frame rate is ours to lower. True when we own the
 *                        encoder and can drop a RAW frame before encode. False on
 *                        UVC H.264 passthrough
 */
void video_rate_ctrl_init(uint32_t camera_fps, uint32_t max_bitrate_bps,
                          uint32_t width, uint32_t height, bool fps_actuable);

/**
 * @brief Create a controller for one sink
 *
 * Starts DISABLED and contributes nothing to arbitration until enabled. Legal
 * before video_rate_ctrl_init() - a transport may come up before the camera does.
 *
 * @param name Short name for logs and stats, e.g. "webrtc" or "putmedia"
 * @param out  Receives the handle
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, or ESP_ERR_NOT_FOUND when
 *         CONFIG_VIDEO_RATE_CTRL_MAX controllers already exist
 */
esp_err_t video_rate_ctrl_create(const char *name, video_rate_ctrl_handle_t *out);

/**
 * @brief Destroy a controller and drop it from arbitration
 *
 * Optional. A transport that starts and stops repeatedly should create its
 * controller once and toggle video_rate_ctrl_enable() instead: keeping the
 * controller alive preserves its learned ladder position across sessions, and
 * avoids a whole class of teardown races.
 *
 * @param h Handle, or NULL (ignored)
 */
void video_rate_ctrl_destroy(video_rate_ctrl_handle_t h);

/**
 * @brief Opt in to (or out of) adaptive rate control for this sink
 *
 * Enable this only from a path that also reports congestion; those reports are
 * the only thing that lets a controller climb back up after backing off. A
 * controller that goes quiet is dropped from arbitration after a timeout rather
 * than pinning the encoder at its last rung - but it keeps its level, so it
 * resumes where it left off instead of re-probing.
 *
 * Enabling starts at the ladder's conservative rung and lets the link earn its
 * way up. Disabling withdraws this controller from arbitration entirely.
 *
 * Order-independent with respect to video_rate_ctrl_init().
 *
 * @param h      Handle, or NULL (ignored)
 * @param enable true to run adaptive control for this sink
 */
void video_rate_ctrl_enable(video_rate_ctrl_handle_t h, bool enable);

/**
 * @brief Report how long one frame took to send (RATE signal)
 *
 * For real-time transports that hand a frame to the network synchronously. The
 * duration is compared against the per-frame budget implied by this controller's
 * current target fps. Ignored while disabled.
 *
 * @param h       Handle, or NULL (ignored)
 * @param send_ms Measured send duration for one video frame, in milliseconds
 */
void video_rate_ctrl_report_send_ms(video_rate_ctrl_handle_t h, uint32_t send_ms);

/**
 * @brief Report upload-queue occupancy (LEVEL signal)
 *
 * For store-and-forward transports whose backpressure shows up as a growing
 * queue rather than a slow individual send. Cheap to call per frame: samples are
 * accumulated and the ladder is only re-evaluated on a slow internal cadence,
 * because the queue cannot respond to a rung change for seconds and a per-report
 * reaction would collapse straight to the floor.
 *
 * Report only when the queue reading is MEANINGFUL. A transport that is dropping
 * or refusing frames upstream of the queue drains it to empty, which reads as
 * perfect health while the stream is in fact broken; suppress the report in that
 * state and use video_rate_ctrl_report_overflow() instead.
 *
 * @param h        Handle, or NULL (ignored)
 * @param used     Buffers currently queued
 * @param capacity Queue capacity; 0 is ignored
 */
void video_rate_ctrl_report_queue(video_rate_ctrl_handle_t h,
                                  uint32_t used, uint32_t capacity);

/**
 * @brief Report that the transport actually lost data to congestion
 *
 * The unambiguous congestion signal: the queue did not just look full, it
 * overflowed and something was discarded. Steps down immediately, bypassing the
 * occupancy path's smoothing and cadence - by the time this fires, the evidence
 * is no longer statistical.
 *
 * @param h Handle, or NULL (ignored)
 */
void video_rate_ctrl_report_overflow(video_rate_ctrl_handle_t h);

/**
 * @brief Report what the encoder actually agreed to (UVC passthrough)
 *
 * The camera clamps freely - and on some cameras refuses outright - so the
 * bitrate it COMMITTED and the bitrate we asked for are different numbers, and
 * only one of them is true.
 *
 * Global rather than per controller: there is one encoder, so there is one
 * committed bitrate, whoever asked for it.
 *
 * @param bps What the encoder reported committing, or 0 to forget
 */
void video_rate_ctrl_report_committed_bitrate_bps(uint32_t bps);

/**
 * @brief Raise the floor the ladder may descend to
 *
 * An encoder that refuses to go below some rate makes every rung under it a lie: the
 * controller keeps asking, the encoder keeps answering with its own number.
 *
 * Absolute bps for the CURRENT resolution, in the same terms as
 * video_rate_ctrl_init()'s max_bitrate_bps. Ignored if it is not above the built-in
 * floor, and clamped so it can never exceed the maximum.
 *
 * @param bps Lowest rate the encoder will actually honour, or 0 to reset
 */
void video_rate_ctrl_set_min_bitrate_bps(uint32_t bps);

/**
 * @brief Declare how long an actuation takes to reach the encoder
 *
 * The queue front-end rate-limits itself on a fixed cadence chosen so a step-down has
 * had time to show up in the signal before the next one is considered. That assumes the
 * encoder responds when told. It does not always: on UVC H.264 passthrough the bitrate
 * reaches the camera over a rate-limited control transfer, or - where even that is
 * ignored - over a stream restart that is deliberately minutes apart.
 *
 * Measured consequence of not declaring it: the ladder walked all eight rungs in three
 * seconds (queue 33% -> 39%) because the cadence let it re-evaluate every 500 ms while
 * the first step-down had not yet reached the camera at all. It then applied its floor
 * rather than a measured step.
 *
 * @param ms Round-trip actuation period, or 0 for an encoder driven directly
 */
void video_rate_ctrl_set_actuation_period_ms(uint32_t ms);

/**
 * @brief Declare whether the bitrate column can actually be actuated
 *
 * Companion to video_rate_ctrl_init()'s @c fps_actuable. Some UVC cameras
 * accept an H.264 bitrate only while the stream is being set up: at runtime the
 * extension-unit probe and commit both succeed and the value is then ignored.
 * Such a camera leaves a passthrough target with NO lever at all - fps is not
 * ours to drop and bitrate is not ours to change - and a controller that keeps
 * walking its ladder is then reporting decisions that reach nothing.
 *
 * Pass false on discovering that, and every controller is frozen at its current
 * rung and reported as inert, so `sink-stats` stops describing a control loop
 * that does not exist. Discovered at runtime, so this is separate from init().
 *
 * @param actuable Whether a bitrate change reaches the encoder
 */
void video_rate_ctrl_set_bitrate_actuable(bool actuable);

/**
 * @brief Cap how far this controller may recover, from a network estimate
 *
 * WebRTC TWCC sender BWE (viewer feedback), in bps. This controller caps its
 * target at min(configured max, this). Pass 0 to clear. Recorded but not acted
 * on while disabled, so it is already in force if this sink later opts in.
 *
 * @param h   Handle, or NULL (ignored)
 * @param bps Estimated available bitrate, or 0 for no network limit
 */
void video_rate_ctrl_set_network_ceiling_bps(video_rate_ctrl_handle_t h, uint32_t bps);

/**
 * @brief Per camera frame: true = encode this frame, false = skip it
 *
 * Skipping is how effective fps is lowered below the camera rate. Reflects the
 * strict minimum target across all eligible controllers. Always true when no
 * controller is enabled.
 *
 * Lock-free; call from the encoder task only.
 */
bool video_rate_ctrl_should_encode(void);

/**
 * @brief Fetch a new encoder bitrate to apply, or 0 if unchanged
 *
 * Reflects the strict minimum across all eligible controllers. The grabber applies it
 * with esp_h264_hw_enc_set_bitrate(), or - on UVC H.264 passthrough, where this is the
 * only lever - to the camera's own encoder.
 *
 * Also where controller staleness is noticed, so a path that does not call
 * should_encode() still drops a silent sink from arbitration. Call from the encoder task
 * only. Declining to call it is safe: the pending target is held until taken, so a
 * caller that rate limits itself delays a change rather than losing it.
 */
uint32_t video_rate_ctrl_pull_bitrate_bps(void);

/**
 * @brief Per-controller snapshot
 *
 * @c level and @c target_fps show where each sink independently wants to run;
 * comparing them against each other is how arbitration is verified.
 */
typedef struct {
    const char *name;
    bool        enabled;
    bool        stale;              /* Excluded: stopped reporting */
    uint32_t    level;              /* Ladder index; 0 is best quality */
    uint32_t    target_fps;
    uint32_t    target_bitrate_bps;
    uint32_t    load_pct;           /* Smoothed load, normalised so 100 == this
                                     * controller's own congestion threshold, so a
                                     * latency sink and a queue sink compare. */
    uint32_t    net_ceiling_bps;    /* 0 = no network limit */
    uint32_t    reports;
} video_rate_ctrl_stats_t;

/**
 * @brief What the one shared encoder is actually doing
 *
 * Separate from the per-controller rows because it is a property of the encoder,
 * not of any sink. @c committed_bps against @c requested_bps is the diagnostic
 * that says whether the ladder's decisions are reaching the hardware at all.
 */
typedef struct {
    uint32_t requested_bps;    /* Last value pushed toward the encoder */
    uint32_t committed_bps;    /* What the encoder said it took; 0 = never reported */
    bool     fps_actuable;     /* Can we drop frames before encode */
    bool     bitrate_actuable; /* Does a bitrate change reach the encoder */
} video_rate_ctrl_encoder_stats_t;

/**
 * @brief Snapshot the shared encoder's side of the story
 *
 * @param out Receives the encoder stats
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_rate_ctrl_get_encoder_stats(video_rate_ctrl_encoder_stats_t *out);

/**
 * @brief Snapshot every controller, plus the arbitrated result
 *
 * @param out       Array to fill
 * @param max       Capacity of @p out
 * @param count     Receives the number of entries written
 * @param enc_fps   If non-NULL, receives the arbitrated encoder fps (0 = native)
 * @param enc_bps   If non-NULL, receives the arbitrated encoder bitrate (0 = native)
 * @return ESP_OK or ESP_ERR_INVALID_ARG
 */
esp_err_t video_rate_ctrl_get_stats(video_rate_ctrl_stats_t *out, size_t max, size_t *count,
                                    uint32_t *enc_fps, uint32_t *enc_bps);

#ifdef __cplusplus
}
#endif
