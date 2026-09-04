/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief KVS combined example: PutMedia recording and WebRTC streaming from one
 *        camera, each independently start/stoppable.
 *
 * This is the example that exercises multi-consumer capture. Both consumers
 * attach to the same camera and the same H.264 encoder as encoded-video sinks:
 *
 *   putmedia.c     - copies each frame into the MKV muxer inside the callback
 *   webrtc_side.c  - copies each frame onto its own queue inside the callback;
 *                    the RTP send runs on kvsGlobalVideo, since writeFrame()
 *                    packetizes inline and must not run on the encoder task
 *
 * Underneath, video_capture_init/start/stop/deinit are reference counted, so the
 * camera comes up on the first consumer and goes down after the last. Try any
 * order: start either, start the other, stop either, restart it.
 *
 * Console:
 *   start-putmedia / stop-putmedia     PutMedia upload
 *   start-webrtc    / stop-webrtc      WebRTC streaming
 *   sink-stats                         per-sink delivery and timing
 */

#include <string.h>
#include "video_capture.h"
#include <stdlib.h>
#include <inttypes.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_cli.h"
#include "wifi_cli.h"
#include "app_storage.h"
#include "app_wifi_prov.h"

#include "esp_webrtc_time.h"
#include "esp_work_queue.h"
#include "video_sink.h"
#include "video_rate_ctrl.h"

#include "esp_console.h"

#include "app_ctrl.h"

static const char *TAG = "kvs_combined";

/* ------------------------------- Console --------------------------------- */

/* Console handlers only enqueue work: starting a session does blocking HTTPS and
 * would otherwise stall the REPL (and risk console reentrancy). */
static void start_putmedia_work_fn(void *priv) { (void) priv; putmedia_start(); }
static void stop_putmedia_work_fn (void *priv) { (void) priv; putmedia_stop();  }
static void start_webrtc_work_fn   (void *priv) { (void) priv; webrtc_side_start(); }
static void stop_webrtc_work_fn    (void *priv) { (void) priv; webrtc_side_stop();  }

static int enqueue(esp_work_fn_t fn, const char *what)
{
    if (esp_work_queue_add_task(fn, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enqueue %s", what);
        return -1;
    }
    return 0;
}

static int start_putmedia_cli(int argc, char **argv)
{
    (void) argc; (void) argv;
    return enqueue(start_putmedia_work_fn, "start-putmedia");
}

static int stop_putmedia_cli(int argc, char **argv)
{
    (void) argc; (void) argv;
    return enqueue(stop_putmedia_work_fn, "stop-putmedia");
}

static int start_webrtc_cli(int argc, char **argv)
{
    (void) argc; (void) argv;
    return enqueue(start_webrtc_work_fn, "start-webrtc");
}

static int stop_webrtc_cli(int argc, char **argv)
{
    (void) argc; (void) argv;
    return enqueue(stop_webrtc_work_fn, "stop-webrtc");
}

/* The numbers that matter for sequential dispatch: max_us and avg_us say who is
 * consuming the shared per-frame budget. Compare max_us against the frame period
 * (33333 us at 30 fps) - anything approaching it is costing everyone frames. */
static int sink_stats_cli(int argc, char **argv)
{
    (void) argc; (void) argv;

    video_sink_stats_t stats[8];
    size_t count = 0;

    if (video_sink_get_stats(stats, sizeof(stats) / sizeof(stats[0]), &count) != ESP_OK) {
        printf("failed to read sink stats\n");
        return -1;
    }

    printf("putmedia: %s   webrtc: %s\n",
           putmedia_is_running() ? "running" : "stopped",
           webrtc_side_is_running() ? "running" : "stopped");

    if (count == 0) {
        printf("no sinks registered\n");
        return 0;
    }

    printf("%-14s %-8s %10s %10s %9s %9s %9s %8s\n",
           "sink", "state", "frames", "gate_held", "max_us", "avg_us", "bytes_kb", "kbps");
    for (size_t i = 0; i < count; i++) {
        printf("%-14s %-8s %10" PRIu32 " %10" PRIu32 " %9" PRIu32 " %9" PRIu32
               " %9" PRIu32 " %8" PRIu32 "\n",
               stats[i].name,
               stats[i].enabled ? "enabled" : "off",
               stats[i].calls, stats[i].gate_held, stats[i].max_us, stats[i].avg_us,
               stats[i].bytes_kb, stats[i].kbps);
    }

    /* Rate control, read right here because the two tables answer one question
     * together: who is asking the shared encoder to slow down, and what did the
     * encoder actually end up running at. Each sink adapts independently; the
     * grabber applies the strict minimum, so enc= should track the worst row. */
    video_rate_ctrl_stats_t rc[8];
    size_t rc_count = 0;
    uint32_t enc_fps = 0, enc_bps = 0;

    if (video_rate_ctrl_get_stats(rc, sizeof(rc) / sizeof(rc[0]), &rc_count,
                                  &enc_fps, &enc_bps) != ESP_OK) {
        printf("failed to read rate-control stats\n");
        return -1;
    }
    if (rc_count == 0) {
        printf("\nno rate controllers\n");
        return 0;
    }

    /* `bar` is the recovery limit: the best rung this controller may climb back into,
     * raised whenever a rung congests. rung == bar means it is being held there
     * deliberately, which is the difference between "cannot recover" and "will not yet". */
    printf("\n%-14s %-8s %5s %4s %5s %10s %6s %10s %9s\n",
           "controller", "state", "rung", "bar", "fps", "bitrate", "load%", "netceil", "reports");
    for (size_t i = 0; i < rc_count; i++) {
        printf("%-14s %-8s %5" PRIu32 " %4" PRIu32 " %5" PRIu32 " %10" PRIu32 " %6" PRIu32 " %10" PRIu32 " %9" PRIu32 "\n",
               rc[i].name,
               rc[i].stale ? "stale" : (rc[i].enabled ? "adapting" : "off"),
               rc[i].level, rc[i].probe_bar_level, rc[i].target_fps, rc[i].target_bitrate_bps,
               rc[i].load_pct, rc[i].net_ceiling_bps, rc[i].reports);
    }
    printf("encoder (strict min of adapting rows): %" PRIu32 " fps / %" PRIu32 " bps\n",
           enc_fps, enc_bps);

    /* What the encoder actually did with all that. The requested/committed pair is
     * the only thing that says whether the ladder above reached the hardware at
     * all - a camera can honour the extension-unit transaction and ignore the
     * value, and then every rung printed above is fiction. */
    video_rate_ctrl_encoder_stats_t enc = {0};
    if (video_rate_ctrl_get_encoder_stats(&enc) == ESP_OK) {
        printf("encoder actuation: fps=%s bitrate=%s | requested %" PRIu32
               " bps, committed %" PRIu32 " bps%s\n",
               enc.fps_actuable ? "actuable" : "FIXED",
               enc.bitrate_actuable ? "actuable" : "IGNORED",
               enc.requested_bps, enc.committed_bps,
               (!enc.fps_actuable && !enc.bitrate_actuable)
                   ? "  <- no lever: rate control is inert" : "");
        if (enc.fps_pin_wanted) {
            printf("frame-rate pin: %s | floor %" PRIu32 " bps (pinned needs %" PRIu32
                   " bps), ladder stops at L%" PRIu32 "\n",
                   enc.fps_pinned ? "held" : "GIVEN UP (bitrate too low for a stable rate)",
                   enc.floor_bps, enc.pinned_floor_bps, enc.deepest_level);
        }
    }
    return 0;
}

static int sink_stats_reset_cli(int argc, char **argv)
{
    (void) argc; (void) argv;
    video_sink_reset_stats();
    printf("sink stats reset\n");
    return 0;
}

/* Flip the camera's rate trade-off without reflashing. A reflash resets the board, every
 * reset taken while the camera streams wedges it, and only a physical replug clears that -
 * so comparing the two settings by reflashing compares two different camera lifetimes. A
 * stream restart keeps the camera powered, which is what makes the comparison honest. */
static int uvc_rate_cli(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: uvc-rate <pin|free>\n"
               "  pin  - fix the frame rate, quality absorbs a bitrate shortfall\n"
               "  free - hold quality, let the camera shed frames\n");
        return 1;
    }
    const bool pin = (strcmp(argv[1], "pin") == 0);
    if (!pin && strcmp(argv[1], "free") != 0) {
        printf("uvc-rate: expected 'pin' or 'free', got '%s'\n", argv[1]);
        return 1;
    }

    const esp_err_t err = video_capture_set_rate_tradeoff(pin);
    if (err != ESP_OK) {
        printf("uvc-rate: %s\n", esp_err_to_name(err));
        return 1;
    }
    /* Queued, not applied: the capture task performs it at the top of its loop, where it
     * holds no buffer. Watch for the `camera H.264 committed:` line to see what the camera
     * actually agreed to - `rc_mode 0x11` means it took the fixed-frame-rate flag, 0x01
     * means it will still drop frames. */
    printf("uvc-rate: %s queued; the stream restarts on the next frame\n",
           pin ? "pin" : "free");
    return 0;
}

/* Companion to uvc-rate: the trade-off has nothing to trade unless the bitrate leaves the
 * camera a choice, so a sweep is needed to find where its frame-rate cliff actually is. */
static int uvc_bitrate_cli(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: uvc-bitrate <bps>\n"
               "  budget at the 1280x720 reference, scaled by pixel count to the\n"
               "  negotiated resolution; 0 restores the configured default.\n"
               "  Restarts the UVC stream and turns rate-control actuation OFF.\n");
        return 1;
    }
    char *end = NULL;
    const unsigned long bps = strtoul(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || bps > 20000000UL) {
        printf("uvc-bitrate: expected a bps value up to 20000000, got '%s'\n", argv[1]);
        return 1;
    }

    const esp_err_t err = video_capture_set_camera_bitrate((uint32_t) bps);
    if (err != ESP_OK) {
        printf("uvc-bitrate: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("uvc-bitrate: %lu queued; the stream restarts on the next frame. Watch the\n"
           "  `camera H.264 committed:` line - the camera clamps freely, and the committed\n"
           "  value is the one to quote, not this request.\n", bps);
    return 0;
}

static void register_cli(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "start-putmedia",
            .help = "Start capturing video and streaming it to KVS via PutMedia",
            .hint = NULL, .func = start_putmedia_cli,
        },
        {
            .command = "stop-putmedia",
            .help = "Stop the PutMedia session (leaves the camera up if WebRTC is running)",
            .hint = NULL, .func = stop_putmedia_cli,
        },
        {
            .command = "start-webrtc",
            .help = "Start KVS WebRTC signaling and streaming",
            .hint = NULL, .func = start_webrtc_cli,
        },
        {
            .command = "stop-webrtc",
            .help = "Stop WebRTC (leaves the camera up if PutMedia is running)",
            .hint = NULL, .func = stop_webrtc_cli,
        },
        {
            .command = "sink-stats",
            .help = "Show per-sink frame counts and callback timing",
            .hint = NULL, .func = sink_stats_cli,
        },
        {
            .command = "sink-stats-reset",
            .help = "Zero the per-sink counters",
            .hint = NULL, .func = sink_stats_reset_cli,
        },
        {
            .command = "uvc-rate",
            .help = "pin|free - what gives way when the camera's bitrate is short",
            .hint = NULL, .func = uvc_rate_cli,
        },
        {
            .command = "uvc-bitrate",
            .help = "<bps> - camera bitrate budget at 720p; restarts the stream",
            .hint = NULL, .func = uvc_bitrate_cli,
        },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = esp_console_cmd_register(&cmds[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register '%s': %s", cmds[i].command, esp_err_to_name(ret));
        }
    }
}

/* --------------------------- Network plumbing ---------------------------- */

#ifdef CONFIG_HOST_USES_STATIC_NETIF
#include "esp_wifi.h"
#include "esp_netif.h"

static esp_err_t create_static_netif_cb(void *ctx)
{
    (void) ctx;
    ESP_LOGI(TAG, "Create netif with static IP");
    /* "Almost" default station, but with an un-flagged DHCP client */
    esp_netif_inherent_config_t netif_cfg;
    memcpy(&netif_cfg, ESP_NETIF_BASE_DEFAULT_WIFI_STA, sizeof(netif_cfg));
    netif_cfg.flags &= ~ESP_NETIF_DHCP_CLIENT;
    esp_netif_config_t cfg_sta = {
        .base  = &netif_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
    };
    esp_netif_t *sta_netif = esp_netif_new(&cfg_sta);
    assert(sta_netif);

    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(sta_netif));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));
    return ESP_OK;
}
#endif

#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "esp_check.h"
#include "esp_hosted_misc.h"

static esp_err_t hosted_bt_prov_start(void *ctx)
{
    (void) ctx;
    ESP_LOGI(TAG, "Initializing BT controller on coprocessor for provisioning");
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_init(), TAG, "BT controller init failed");
    ESP_RETURN_ON_ERROR(esp_hosted_bt_controller_enable(), TAG, "BT controller enable failed");
    return ESP_OK;
}

static void hosted_bt_prov_end(void *ctx)
{
    (void) ctx;
    ESP_LOGI(TAG, "Deinitializing BT controller on coprocessor");
    esp_hosted_bt_controller_disable();
    esp_hosted_bt_controller_deinit(true);
}
#endif

#ifdef CONFIG_SLAVE_FLASHER_ENABLE
#include "slave_flasher.h"
#endif

/* -------------------------------- app_main ------------------------------- */

void app_main(void)
{
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#ifdef CONFIG_SLAVE_FLASHER_ENABLE
    /* Program the attached C6/C5 co-processor before bringing up esp_hosted /
     * Wi-Fi, so the network path is available once the slave is running. */
    ret = flash_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to flash slave co-processor: %s", esp_err_to_name(ret));
        return;
    }
#endif

    ESP_LOGI(TAG, "KVS combined example: PutMedia + WebRTC on one camera");

    /* The console handlers offload to the work queue, so it has to exist before
     * the commands are registered - the prompt comes up long before Wi-Fi is
     * associated, and start-putmedia is typeable from that moment.
     *
     * Size it for its most demanding user, which is WebRTC, not PutMedia.
     * app_webrtc dispatches peer-connection setup onto this same queue, and
     * kvs_initializePeerConnection() puts an RtcConfiguration on the stack -
     * several KB of ICE-server URIs. app_webrtc_init() asks for 32 KB, but
     * esp_work_queue_init_with_config() returns early once the queue exists
     * (esp_work_queue.c:123) and the task has already been created by our
     * esp_work_queue_start(), so its request is silently ignored and only this
     * value takes effect. examples/put_media gets away with 12 KB because
     * nothing there runs WebRTC on the queue; here that overflows the stack the
     * moment a peer sends an offer. Keep this >= app_webrtc's 32 KB. */
    esp_work_queue_config_t work_queue_config = ESP_WORK_QUEUE_CONFIG_DEFAULT();
    work_queue_config.stack_size = 32 * 1024;
    work_queue_config.size = 128;
    if (esp_work_queue_init_with_config(&work_queue_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize work queue");
        return;
    }
    if (esp_work_queue_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start work queue");
        return;
    }

    esp_cli_start();
    wifi_register_cli();
    register_cli();

    app_wifi_prov_config_t net_cfg = APP_NETWORK_CONFIG_DEFAULT();
#ifdef CONFIG_HOST_USES_STATIC_NETIF
    net_cfg.pre_wifi_init_cb = create_static_netif_cb;
    net_cfg.skip_default_sta_netif = true;
#endif
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    net_cfg.prov_start_cb = hosted_bt_prov_start;
    net_cfg.prov_end_cb   = hosted_bt_prov_end;
#endif
    ret = app_wifi_prov_init(&net_cfg);
    if (ret == ESP_ERR_TIMEOUT) {
        /* Wi-Fi not connected within wifi_connect_timeout_ms - common on the
         * P4 + C6 hosted path, where co-processor association takes longer than
         * the timeout. The connection continues in the background. */
        ESP_LOGW(TAG, "Wi-Fi not connected yet; continuing (will connect in background)");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi provisioning init failed: %s", esp_err_to_name(ret));
        return;
    }

    app_storage_init();

    /* SigV4 signatures are rejected if the clock is off, so the time sync has to
     * complete before the first request from either side. */
    esp_webrtc_time_sntp_time_sync_and_wait();

    ESP_LOGI(TAG, "Ready. Commands: start-putmedia, start-webrtc, stop-putmedia, "
                  "stop-webrtc, sink-stats");

    enqueue(start_webrtc_work_fn, "start-webrtc");
}
