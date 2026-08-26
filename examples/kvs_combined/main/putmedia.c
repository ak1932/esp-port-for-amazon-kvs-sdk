/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief PutMedia side of the combined example.
 *
 * Capture -> MKV mux -> KVS PutMedia. Frames arrive through an encoded-video
 * sink callback rather than a pump task: the sink runs on the encoder task,
 * sequentially with every other sink, and the frame it receives is borrowed for
 * the duration of the call only.
 *
 * The callback feeds the muxer only while a PutMedia session is actually open -
 * which excludes the seconds each reopen takes, not just the initial connect.
 * Everything downstream of it - the frame copy, the Annex-B to AVCC rewrite, the
 * muxer's stream insert - is wasted otherwise, and it is wasted on the encoder
 * task, whose per-frame budget is shared with WebRTC.
 *
 * Derived from examples/put_media, with the credentials moved to Kconfig so the
 * two halves of this example share one place to configure AWS.
 */

#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "media_stream.h"
#include "video_capture.h"
#include "video_sink.h"

#include "kvs/mkvapp.h"
#include "kvs/put_media.h"

#include "app_ctrl.h"

static const char *TAG = "putmedia";

/* ------------------------------- AWS config ------------------------------ */

#define AWS_REGION              CONFIG_AWS_DEFAULT_REGION
#define AWS_KVS_STREAM_NAME     CONFIG_KVS_COMBINED_STREAM_NAME
#define AWS_KVS_SERVICE         "kinesisvideo"
/* Control plane. PutMedia resolves the data endpoint itself via GetDataEndpoint
 * and then POSTs fragments there. */
#define AWS_KVS_HOST            AWS_KVS_SERVICE "." AWS_REGION ".amazonaws.com"
#define AWS_KVS_RETENTION_HOURS CONFIG_KVS_COMBINED_RETENTION_HOURS

#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
#define AWS_ACCESS_KEY_ID     ""
#define AWS_SECRET_ACCESS_KEY ""
#define AWS_SESSION_TOKEN     ""
#else
#define AWS_ACCESS_KEY_ID CONFIG_AWS_ACCESS_KEY_ID
#define AWS_SECRET_ACCESS_KEY CONFIG_AWS_SECRET_ACCESS_KEY
#define AWS_SESSION_TOKEN CONFIG_AWS_SESSION_TOKEN
#endif

/* ----------------------------- Capture config ---------------------------- */

#define VIDEO_WIDTH   CONFIG_KVS_COMBINED_VIDEO_WIDTH
#define VIDEO_HEIGHT  CONFIG_KVS_COMBINED_VIDEO_HEIGHT
#define VIDEO_FPS     CONFIG_KVS_COMBINED_VIDEO_FPS
#define VIDEO_BITRATE CONFIG_KVS_COMBINED_VIDEO_BITRATE

/* Spare bytes appended to each frame copy. mkv_app_add_frame() rewrites the
 * frame from Annex-B to AVCC *in place*, and a 3-byte start code becomes a
 * 4-byte length prefix, so the buffer must be able to grow by one byte per NAL
 * unit. 64 bytes covers far more NAL units than a single frame ever carries. */
#define ANNEXB_TO_AVCC_HEADROOM 64

/* -------------------------------- State ---------------------------------- */

static mkv_app_handle                s_mkv;
static media_stream_video_capture_t *s_video_if;
static video_capture_handle_t        s_video_handle;
static video_sink_handle_t           s_video_sink;
static volatile bool                 s_running;

/* Upload accounting, reported once a second from the muxer callback.
 * "lost" and "skipped" are different failures and must not be added together.
 * A lost buffer is damage: it never reached PutMedia, and its absence is what
 * breaks the cluster it belonged to. A skipped buffer is the fallout - PutMedia
 * refuses the rest of that cluster on purpose, because sending it would only
 * produce a fragment KVS rejects. One loss typically causes tens of skips. */
static uint32_t s_n_sent, s_n_lost, s_n_skipped;
static uint32_t s_bytes_sent, s_bytes_lost, s_bytes_skipped;
static int64_t  s_stat_time;
static put_media_err_t s_last_err;

/* Capture gate. The sink is enabled for the whole time PutMedia runs, but
 * frames are only worth muxing once the PutMedia session is actually open -
 * see the reasoning in video_sink_on_frame(). s_await_keyframe re-arms whenever
 * the gate shuts, s_gate_shut deduplicates the log to one line per edge, and
 * s_gate_skipped counts the frames the gate refused. */
static bool     s_await_keyframe = true;
static bool     s_gate_shut;
static uint32_t s_gate_skipped;

/* Session-open retry. */
#define PUT_MEDIA_MAX_RETRIES     3
#define PUT_MEDIA_RETRY_BASE_MS   2000

static esp_timer_handle_t s_retry_timer;
static int                s_retry_attempts;

/* Runs on the esp_timer task. put_media_start() only checks state and posts a
 * task notification, so it is safe from here. */
static void put_media_retry_cb(void *arg)
{
    (void) arg;

    if (!s_running || put_media_is_started()) {
        return;
    }

    ESP_LOGW(TAG, "Retrying PutMedia session open (attempt %d/%d)",
             s_retry_attempts, PUT_MEDIA_MAX_RETRIES);

    if (put_media_start() != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "PutMedia retry could not be signalled");
    }
}

/* PutMedia stores these pointers rather than copying the strings, so they must
 * stay valid for the lifetime of the session - string literals qualify. */
static put_media_err_t fetch_credentials(uint64_t customData,
                                         const char **pAccessKey, uint32_t *pAccessKeyLen,
                                         const char **pSecretKey, uint32_t *pSecretKeyLen,
                                         const char **pSessionToken, uint32_t *pSessionTokenLen,
                                         uint32_t *pExpiration)
{
    (void) customData;

    *pAccessKey    = AWS_ACCESS_KEY_ID;
    *pAccessKeyLen = strlen(AWS_ACCESS_KEY_ID);

    *pSecretKey    = AWS_SECRET_ACCESS_KEY;
    *pSecretKeyLen = strlen(AWS_SECRET_ACCESS_KEY);

    if (strlen(AWS_SESSION_TOKEN) > 0) {
        *pSessionToken    = AWS_SESSION_TOKEN;
        *pSessionTokenLen = strlen(AWS_SESSION_TOKEN);
    } else {
        *pSessionToken    = NULL;
        *pSessionTokenLen = 0;
    }

    /* Static credentials never expire as far as this example is concerned. A
     * real provider would return the STS expiry so the session is refreshed. */
    *pExpiration = UINT32_MAX;

    return PUT_MEDIA_ERRNO_NONE;
}

static put_media_err_t get_mkv_ebml_hdr(mkv_buf_info_t *mkv_buf_info)
{
    if (mkv_app_emit_ebml_header(s_mkv, mkv_buf_info) != MKV_ERRNO_NONE) {
        ESP_LOGE(TAG, "MKV EBML header not available yet");
        return PUT_MEDIA_ERROR_INVALID_STATE;
    }
    return PUT_MEDIA_ERRNO_NONE;
}

/* Called by the muxer task for every MKV element it produces. PutMedia deep
 * copies the buffer, so ownership stays with the muxer. */
static mkv_err_t on_mkv_generated(mkv_app_handle handle, mkv_buf_info_t mkv_buf_info, void *pAppData)
{
    (void) handle;
    (void) pAppData;

    if (!put_media_is_started()) {
        return MKV_ERRNO_NONE;
    }

    put_media_err_t err = put_media_enqueue(mkv_buf_info);
    if (err == PUT_MEDIA_ERRNO_NONE) {
        s_n_sent++;
        s_bytes_sent += mkv_buf_info.uDataLen;
    } else if (err == PUT_MEDIA_ERROR_AWAITING_RESYNC) {
        /* Deliberately refused: the session is reopening, or a previous loss left
         * PutMedia waiting for the next cluster boundary. Whatever caused that was
         * already counted, so this is not new loss. */
        s_n_skipped++;
        s_bytes_skipped += mkv_buf_info.uDataLen;
    } else {
        /* Real loss. TIMEOUT means the upload queue is full, i.e. PutMedia drains
         * slower than the encoder produces; OUT_OF_MEMORY means no PSRAM for the
         * copy. Keep muxing - PutMedia resumes at the next cluster boundary. */
        s_n_lost++;
        s_bytes_lost += mkv_buf_info.uDataLen;
        s_last_err = err;
    }

    int64_t now = esp_timer_get_time();
    if (now - s_stat_time >= 1000000) {
        if (s_n_lost > 0 || s_n_skipped > 0) {
            ESP_LOGW(TAG,
                     "upload: sent %" PRIu32 " buf/%" PRIu32 " B | LOST %" PRIu32 " buf/%" PRIu32
                     " B (err %d) | skipped %" PRIu32 " buf/%" PRIu32 " B | queued %u",
                     s_n_sent, s_bytes_sent, s_n_lost, s_bytes_lost, s_last_err,
                     s_n_skipped, s_bytes_skipped, put_media_get_pending_count());
        } else {
            ESP_LOGD(TAG, "upload: sent %" PRIu32 " buf/%" PRIu32 " B | queued %u",
                     s_n_sent, s_bytes_sent, put_media_get_pending_count());
        }
        s_n_sent = s_n_lost = s_n_skipped = 0;
        s_bytes_sent = s_bytes_lost = s_bytes_skipped = 0;
        s_stat_time = now;
    }

    return MKV_ERRNO_NONE;
}

static void put_media_event_handler(const put_media_event_data_t *event_data, void *user_ctx)
{
    (void) user_ctx;

    switch (event_data->event) {
    case PUT_MEDIA_ON_START:
        ESP_LOGI(TAG, "PutMedia started - streaming to '%s'", AWS_KVS_STREAM_NAME);
        s_retry_attempts = 0;
        break;
    case PUT_MEDIA_ON_STOP:
        ESP_LOGI(TAG, "PutMedia stopped");
        break;
    case PUT_MEDIA_ON_ERROR:
        /* Retry if put media did not start */
        ESP_LOGE(TAG, "PutMedia error: %d", event_data->data.error_code);
        if (!put_media_is_started() && s_running) {
            if (s_retry_timer != NULL && s_retry_attempts < PUT_MEDIA_MAX_RETRIES) {
                s_retry_attempts++;
                uint64_t delay_ms = (uint64_t) PUT_MEDIA_RETRY_BASE_MS << (s_retry_attempts - 1);
                ESP_LOGW(TAG, "Session did not open; retrying in %" PRIu64 " ms (%d/%d)",
                         delay_ms, s_retry_attempts, PUT_MEDIA_MAX_RETRIES);
                esp_timer_stop(s_retry_timer);
                esp_timer_start_once(s_retry_timer, delay_ms * 1000);
            } else {
                ESP_LOGE(TAG, "Session did not open after %d attempts; frames are being "
                              "dropped at the sink. Run stop-putmedia then start-putmedia.",
                         s_retry_attempts);
            }
        }
        break;
    case PUT_MEDIA_ON_ACK_RECV:
        ESP_LOGI(TAG, "Fragment ack: type=%d timecode=%" PRIu64 " errorId=%u",
                 event_data->data.ack.eventType, event_data->data.ack.uFragmentTimecode,
                 event_data->data.ack.uErrorId);
        /* eError carries a KVS fragment error id; 4006 is "invalid MKV data",
         * which is what an incomplete cluster looks like from the far end. */
        break;
    default:
        break;
    }
}

/* Receives every encoded H.264 frame and feeds it to the muxer, whose own task
 * turns them into MKV clusters and calls on_mkv_generated(). 
 * Runs on encoder task with every other sink */
static void video_sink_on_frame(const video_frame_t *frame, void *user)
{
    (void) user;

    if (!s_running || s_mkv == NULL) {
        return;
    }

    /* Nothing downstream can use this frame unless a session is open right now. */
    if (!put_media_is_session_live()) {
        if (!s_gate_shut) {
            ESP_LOGI(TAG, "No PutMedia session, holding frames");
            s_gate_shut = true;
        }
        /* Resume on a keyframe, not mid-GOP. */
        s_await_keyframe = true;
        s_gate_skipped++;
        return;
    }

    if (s_await_keyframe) {
        if (frame->type != VIDEO_FRAME_TYPE_I) {
            s_gate_skipped++;
            return;
        }
        s_await_keyframe = false;
        s_gate_shut = false;
        ESP_LOGI(TAG, "Feeding the muxer, opened on keyframe (held %" PRIu32 " frames)",
                 s_gate_skipped);
        s_gate_skipped = 0;
    }

    /* The frame is borrowed and dies when this returns, and mkv_app_add_frame()
     * takes ownership of what it is given and rewrites it in place. So hand over
     * a private copy with headroom; the muxer frees it, error path included. */
    size_t capacity = frame->len + ANNEXB_TO_AVCC_HEADROOM;
    uint8_t *copy = heap_caps_malloc_prefer(capacity, 2,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        ESP_LOGW(TAG, "Out of memory for a %" PRIu32 "-byte frame, dropping", frame->len);
        return;
    }
    memcpy(copy, frame->buffer, frame->len);

    /* frame->timestamp is microseconds from esp_timer, stamped once by the
     * encoder so every sink agrees. The muxer makes cluster timecodes relative
     * to the first frame it sees, and PutMedia sends the wall-clock epoch
     * separately in x-amzn-producer-start-timestamp. */
    mkv_err_t err = mkv_app_add_frame(s_mkv, copy, frame->len, capacity,
                                      frame->timestamp / 1000, TRACK_VIDEO);
    if (err != MKV_ERRNO_NONE) {
        ESP_LOGW(TAG, "mkv_app_add_frame failed: %d", err);
    }
}

bool putmedia_is_running(void)
{
    return s_running;
}

esp_err_t putmedia_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "PutMedia already running");
        return ESP_OK;
    }

    if (strlen(AWS_ACCESS_KEY_ID) == 0) {
        ESP_LOGE(TAG, "No AWS credentials. PutMedia signs its own requests and needs static "
                      "keys: set CONFIG_IOT_CORE_ENABLE_CREDENTIALS=n and fill in "
                      "CONFIG_AWS_ACCESS_KEY_ID / _SECRET_ACCESS_KEY / _SESSION_TOKEN.");
        return ESP_ERR_INVALID_STATE;
    }

    s_video_if = media_stream_get_video_capture_if();
    if (s_video_if == NULL) {
        ESP_LOGE(TAG, "No video capture interface");
        return ESP_FAIL;
    }

    /* Reference counted: if WebRTC already brought the camera up this joins it,
     * keeping the existing profile. Whoever gets here first sets it. */
    video_capture_config_t video_config = {
        .codec = VIDEO_CODEC_H264,
        .resolution = { .width = VIDEO_WIDTH, .height = VIDEO_HEIGHT, .fps = VIDEO_FPS },
        .quality = 80,
        .bitrate = VIDEO_BITRATE,
        .codec_specific = NULL,
    };
    esp_err_t ret = s_video_if->init(&video_config, &s_video_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Video capture init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* No video_rate_ctrl_enable() here */

    /* STREAM_POLICY_RING_BUFFER is a last-resort memory bound, NOT the working
     * buffer - the PutMedia upload queue is. Eviction here is destructive:
     * prvStreamFlushHeadUntilMem() drops oldest-first with no cluster awareness,
     * so the first thing it discards is the cluster frame at the head, orphaning
     * that cluster's blocks. Size it so that never happens in practice. */
    mkv_app_config_t mkv_config = {
        .stream_policy = STREAM_POLICY_RING_BUFFER,
        .ring_buf_mem  = 4 * 1024 * 1024,
    };
    s_mkv = mkv_app_init(&mkv_config);
    if (s_mkv == NULL) {
        ESP_LOGE(TAG, "MKV app init failed");
        goto err_video;
    }

    on_mkv_generated_callback_info_t mkv_cb = {
        .onMkvGeneratedCallback = on_mkv_generated,
        .pAppData = NULL,
    };
    if (mkv_app_set_on_mkv_generated_callbacks(s_mkv, &mkv_cb, 1) != MKV_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to set MKV callback");
        goto err_mkv;
    }

    put_media_config_t put_media_config = {
        .pcHost                  = AWS_KVS_HOST,
        .pcRegion                = AWS_REGION,
        .pcService               = AWS_KVS_SERVICE,
        .pcStreamName            = AWS_KVS_STREAM_NAME,
        .get_mkv_ebml_hdr_func   = get_mkv_ebml_hdr,
        .kvs_fetch_credentials_cb = fetch_credentials,
        .uDataRetentionHours     = AWS_KVS_RETENTION_HOURS,
        .event_handler           = put_media_event_handler,
        .event_handler_user_ctx  = NULL,
    };
    if (put_media_init(put_media_config) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "PutMedia init failed");
        goto err_mkv;
    }

    if (mkv_app_start(s_mkv) != MKV_ERRNO_NONE) {
        ESP_LOGE(TAG, "MKV app start failed");
        goto err_putmedia;
    }

    if (s_video_if->start(s_video_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Video capture start failed");
        goto err_mkv_started;
    }

    s_running = true;
    s_n_sent = s_n_lost = s_n_skipped = 0;
    s_bytes_sent = s_bytes_lost = s_bytes_skipped = 0;
    s_stat_time = esp_timer_get_time();
    s_await_keyframe = true;
    s_gate_shut = false;
    s_gate_skipped = 0;

    if (s_video_sink == NULL) {
        const video_sink_config_t sink_cfg = {
            .name     = "putmedia",
            .on_frame = video_sink_on_frame,
            .user     = NULL,
        };
        if (video_sink_register(&sink_cfg, &s_video_sink) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register video sink");
            s_running = false;
            goto err_capture;
        }
    }
    if (video_sink_set_enabled(s_video_sink, true) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable video sink");
        s_running = false;
        goto err_capture;
    }

    if (s_retry_timer == NULL) {
        const esp_timer_create_args_t retry_args = {
            .callback = put_media_retry_cb,
            .name     = "putmedia_retry",
        };
        if (esp_timer_create(&retry_args, &s_retry_timer) != ESP_OK) {
            /* Not fatal: without it a failed open is simply terminal, as before. */
            ESP_LOGW(TAG, "Could not create PutMedia retry timer; session open will not retry");
            s_retry_timer = NULL;
        }
    }
    s_retry_attempts = 0;


    if (put_media_start() != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "PutMedia start failed");
        goto err_sink;
    }

    ESP_LOGI(TAG, "Streaming to KVS stream '%s' in %s", AWS_KVS_STREAM_NAME, AWS_REGION);
    return ESP_OK;

err_sink:
    s_running = false;
    /* Before put_media_deinit() below, so a pending retry cannot fire into a
     * torn-down PutMedia. The callback also re-checks s_running. */
    if (s_retry_timer != NULL) {
        esp_timer_stop(s_retry_timer);
    }
    video_sink_set_enabled(s_video_sink, false);
err_capture:
    s_video_if->stop(s_video_handle);
err_mkv_started:
    mkv_app_stop(s_mkv);
err_putmedia:
    put_media_deinit();
err_mkv:
    mkv_app_deinit(s_mkv);
    s_mkv = NULL;
err_video:
    s_video_if->deinit(s_video_handle);
    s_video_handle = NULL;
    return ESP_FAIL;
}

esp_err_t putmedia_stop(void)
{
    if (!s_running) {
        ESP_LOGW(TAG, "PutMedia not running");
        return ESP_OK;
    }

    /* Stop feeding first, so nothing is added to a torn-down muxer. Disabling
     * the sink is synchronous with respect to the dispatcher: it will not be
     * inside our callback once this returns. */
    s_running = false;
    video_sink_set_enabled(s_video_sink, false);

    /* Reference counted - the camera keeps running if WebRTC still holds it. */
    s_video_if->stop(s_video_handle);
    put_media_stop();
    mkv_app_stop(s_mkv);

    put_media_deinit();
    mkv_app_deinit(s_mkv);
    s_mkv = NULL;

    s_video_if->deinit(s_video_handle);
    s_video_handle = NULL;

    ESP_LOGI(TAG, "PutMedia stopped");
    return ESP_OK;
}
