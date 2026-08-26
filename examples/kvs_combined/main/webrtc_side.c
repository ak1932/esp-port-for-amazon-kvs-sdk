/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief WebRTC side of the combined example.
 *
 * Streams the same encoded H.264 PutMedia archives, over KVS WebRTC.
 *
 * The sink itself lives in components/kvs_webrtc/src/kvs_media.c, registered
 * under the name "webrtc" when transmission starts: its callback copies each
 * frame onto a short queue and returns, and the RTP send runs on the
 * kvsGlobalVideo thread. This file only drives the start/stop lifecycle.
 *
 * Derived from examples/webrtc_classic, wrapped in start/stop so it can be
 * brought up and torn down independently of PutMedia.
 */

#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include "esp_log.h"

#include "app_webrtc.h"
#include "kvs_signaling.h"
#include "kvs_peer_connection.h"
#include "media_stream.h"
#include "video_capture.h"

#include "app_ctrl.h"

#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
#define AWS_ACCESS_KEY_ID     ""
#define AWS_SECRET_ACCESS_KEY ""
#define AWS_SESSION_TOKEN     ""
#else
#define AWS_ACCESS_KEY_ID CONFIG_AWS_ACCESS_KEY_ID
#define AWS_SECRET_ACCESS_KEY CONFIG_AWS_SECRET_ACCESS_KEY
#define AWS_SESSION_TOKEN CONFIG_AWS_SESSION_TOKEN
#endif

static const char *TAG = "webrtc_side";

static bool s_running;
static bool s_event_cb_registered;

// TODO: see if this is required
/* Camera lease held for the whole start-webrtc..stop-webrtc window.
 *
 * kvs_media takes its own reference per *session* and drops it when the last
 * peer goes away, which used to tear the camera down between peers. On a UVC
 * camera that teardown is unrecoverable: esp_video_if_start() must free the
 * previous session's capture buffers (stopping the stream kills the underlying
 * uvc_host stream, so carried-over buffers reference dead queues) and then
 * re-REQBUFS three buffers of width*height*2 bytes - 11.9 MB at 1080p, which
 * cannot be shrunk because a UVC camera refuses to stream when the host offers
 * less than its declared frame size. After one session's worth of WebRTC/TLS
 * allocation there is no longer three contiguous ~4 MB blocks in a 16 MB PSRAM
 * heap, so the second peer connection died on
 * "Failed to require buffers, errno: 12" (ENOMEM) and streamed without video.
 *
 * Holding one reference here keeps that 11.9 MB allocated exactly once, so
 * per-session init/start just joins it. It also moves the ~1.5 s camera
 * bring-up out of the offer handler, which runs on the shared work queue. */
static video_capture_handle_t s_camera_lease;

static void camera_lease_acquire(media_stream_video_capture_t *video_capture)
{
    if (video_capture == NULL || s_camera_lease != NULL) {
        return;
    }

    video_capture_config_t cfg = {
        .codec = VIDEO_CODEC_H264,
        .resolution = { .width  = CONFIG_KVS_COMBINED_VIDEO_WIDTH,
                        .height = CONFIG_KVS_COMBINED_VIDEO_HEIGHT,
                        .fps    = CONFIG_KVS_COMBINED_VIDEO_FPS },
        .quality = 80,
        .bitrate = CONFIG_KVS_COMBINED_VIDEO_BITRATE,
        .codec_specific = NULL,
    };

    /* Non-fatal: WebRTC already tolerates a missing camera (it warns and streams
     * without video), and signaling is still worth having. */
    esp_err_t ret = video_capture->init(&cfg, &s_camera_lease);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera lease init failed (%s) - continuing; the camera will be "
                      "brought up per session instead", esp_err_to_name(ret));
        s_camera_lease = NULL;
        return;
    }
    ret = video_capture->start(s_camera_lease);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Camera lease start failed (%s) - releasing it", esp_err_to_name(ret));
        video_capture->deinit(s_camera_lease);
        s_camera_lease = NULL;
        return;
    }
    ESP_LOGI(TAG, "Camera lease held for the WebRTC session window");
}

static void camera_lease_release(void)
{
    if (s_camera_lease == NULL) {
        return;
    }
    media_stream_video_capture_t *video_capture = media_stream_get_video_capture_if();
    if (video_capture != NULL) {
        video_capture->stop(s_camera_lease);
        video_capture->deinit(s_camera_lease);
    }
    s_camera_lease = NULL;
    ESP_LOGI(TAG, "Camera lease released");
}

bool webrtc_side_is_running(void)
{
    return s_running;
}

static void app_webrtc_event_handler(app_webrtc_event_data_t *event_data, void *user_ctx)
{
    (void) user_ctx;

    if (event_data == NULL) {
        return;
    }

    switch (event_data->event_id) {
    case APP_WEBRTC_EVENT_INITIALIZED:
        ESP_LOGI(TAG, "[KVS] WebRTC initialized");
        break;
    case APP_WEBRTC_EVENT_DEINITIALIZING:
        ESP_LOGI(TAG, "[KVS] WebRTC deinitializing");
        break;
    case APP_WEBRTC_EVENT_SIGNALING_CONNECTED:
        ESP_LOGI(TAG, "[KVS] Signaling connected");
        break;
    case APP_WEBRTC_EVENT_SIGNALING_DISCONNECTED:
        ESP_LOGW(TAG, "[KVS] Signaling disconnected");
        break;
    case APP_WEBRTC_EVENT_PEER_CONNECTED:
        ESP_LOGI(TAG, "[KVS] Peer connected: %s", event_data->peer_id);
        break;
    case APP_WEBRTC_EVENT_PEER_DISCONNECTED:
        ESP_LOGI(TAG, "[KVS] Peer disconnected: %s", event_data->peer_id);
        break;
    default:
        ESP_LOGD(TAG, "[KVS] event %d", (int) event_data->event_id);
        break;
    }
}

esp_err_t webrtc_side_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "WebRTC already running");
        return ESP_OK;
    }

    /* Registered before init so no event is missed. Only once - the callback
     * outlives a stop/start cycle. */
    if (!s_event_cb_registered) {
        if (app_webrtc_register_event_callback(app_webrtc_event_handler, NULL) != 0) {
            ESP_LOGE(TAG, "Failed to register KVS event callback");
        } else {
            s_event_cb_registered = true;
        }
    }

    /* The capture interfaces. app_webrtc/kvs_media call init/start on these
     * itself, and those are reference counted inside media_stream, so if
     * PutMedia already has the camera up this simply joins it. kvs_media also
     * registers its own encoded-video sink at that point. */
    media_stream_video_capture_t *video_capture = media_stream_get_video_capture_if();
    media_stream_audio_capture_t *audio_capture = media_stream_get_audio_capture_if();
    media_stream_video_player_t  *video_player  = NULL;
    media_stream_audio_player_t  *audio_player  = NULL;

#ifdef CONFIG_ESP_P4_CORE_BOARD
    audio_capture = NULL;
    audio_player  = NULL;
#endif

#ifndef CONFIG_MEDIA_STREAM_ENABLE_VIDEO_PLAYER
    /* Receive-path player is opt-in. When disabled the media_stream interface is
     * a logging stub - do not wire it in, so received video is simply dropped. */
    video_player = NULL;
#endif

    if (video_capture == NULL) {
        ESP_LOGW(TAG, "Video capture not available - continuing without video capture");
    }

    /* Bring the camera up now and keep it up, rather than letting it come and go
     * with each peer connection. */
    camera_lease_acquire(video_capture);

    static kvs_signaling_config_t kvs_signaling_cfg = {0};
    kvs_signaling_cfg.pChannelName = CONFIG_AWS_KVS_CHANNEL_NAME;

#ifdef CONFIG_IOT_CORE_ENABLE_CREDENTIALS
    kvs_signaling_cfg.useIotCredentials         = true;
    kvs_signaling_cfg.iotCoreCredentialEndpoint = CONFIG_AWS_IOT_CORE_CREDENTIAL_ENDPOINT;
    kvs_signaling_cfg.iotCoreCert               = CONFIG_AWS_IOT_CORE_CERT;
    kvs_signaling_cfg.iotCorePrivateKey         = CONFIG_AWS_IOT_CORE_PRIVATE_KEY;
    kvs_signaling_cfg.iotCoreRoleAlias          = CONFIG_AWS_IOT_CORE_ROLE_ALIAS;
    kvs_signaling_cfg.iotCoreThingName          = CONFIG_AWS_IOT_CORE_THING_NAME;
#else
    kvs_signaling_cfg.useIotCredentials = false;
    kvs_signaling_cfg.awsAccessKey      = AWS_ACCESS_KEY_ID;
    kvs_signaling_cfg.awsSecretKey      = AWS_SECRET_ACCESS_KEY;
    kvs_signaling_cfg.awsSessionToken   = AWS_SESSION_TOKEN;
#endif

    kvs_signaling_cfg.awsRegion  = CONFIG_AWS_DEFAULT_REGION;
    kvs_signaling_cfg.caCertPath = "/spiffs/certs/cacert.pem";

    app_webrtc_config_t app_webrtc_config = APP_WEBRTC_CONFIG_DEFAULT();
    app_webrtc_config.signaling_client_if = kvs_signaling_client_if_get();
    app_webrtc_config.signaling_cfg       = &kvs_signaling_cfg;
    app_webrtc_config.peer_connection_if  = kvs_peer_connection_if_get();
    app_webrtc_config.video_capture       = video_capture;
    app_webrtc_config.audio_capture       = audio_capture;
    app_webrtc_config.video_player        = video_player;
    app_webrtc_config.audio_player        = audio_player;

    ESP_LOGI(TAG, "Starting WebRTC on channel '%s' (%s)",
             CONFIG_AWS_KVS_CHANNEL_NAME, CONFIG_AWS_DEFAULT_REGION);

    WEBRTC_STATUS status = app_webrtc_init(&app_webrtc_config);
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "app_webrtc_init failed: 0x%08" PRIx32, (uint32_t) status);
        return ESP_FAIL;
    }

    app_webrtc_enable_media_reception(true);

    status = app_webrtc_run();
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "app_webrtc_run failed: 0x%08" PRIx32, (uint32_t) status);
        app_webrtc_terminate();
        return ESP_FAIL;
    }

    s_running = true;
    ESP_LOGI(TAG, "WebRTC started");
    return ESP_OK;
}

esp_err_t webrtc_side_stop(void)
{
    if (!s_running) {
        ESP_LOGW(TAG, "WebRTC not running");
        return ESP_OK;
    }

    /* Tears down sessions and the signaling link, and releases its reference on
     * the capture pipeline. The camera keeps running if PutMedia still holds
     * one. */
    WEBRTC_STATUS status = app_webrtc_terminate();
    if (status != WEBRTC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "app_webrtc_terminate failed: 0x%08" PRIx32, (uint32_t) status);
    }

    /* After app_webrtc_terminate(), so sessions have already dropped their own
     * references - this releases the last one unless PutMedia holds it. */
    camera_lease_release();

    s_running = false;
    ESP_LOGI(TAG, "WebRTC stopped");
    return ESP_OK;
}
