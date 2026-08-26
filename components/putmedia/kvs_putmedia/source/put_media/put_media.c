#include "azure_c_shared_utility/crt_abstractions.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "kvs/iot_credential_provider.h"
#include "kvs/port.h"
#include "kvs/put_media_errors.h"
#include "kvs/restapi.h"
#include "net/http_helper.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kvs/put_media.h"
#include "esp_timer.h"
#include "portmacro.h"

#define VIDEO_CODEC_NAME "V_MPEG4/ISO/AVC"
#define VIDEO_TRACK_NAME "ak video track"

#define DEFAULT_CONNECTION_TIMEOUT_MS (10 * 1000)
#define DEFAULT_DATA_RETENTION_IN_HOURS (2)
/*
 * esp_http_client has one timeout for both directions, so this governs every
 * chunk write on the streaming request as well as the 100->200 header fetch.
 *
 * It was 2000 ms, which turned ordinary backpressure into a session kill: on a
 * saturated uplink the payload write returns immediately into the send buffer and
 * the *next* 13-byte size line or trailer blocks until the window opens. Past the
 * timeout, prvCloseOpenChunk() declares the stream unusable and the session is
 * reopened - ~5 s of not draining, plus a resync, which deepens the very backlog
 * that caused the stall. Blocking here is the correct response to a slow link.
 *
 * Fragment acks are unaffected: Kvs_putMediaReadFragmentAck() brackets its read
 * with its own 1 ms timeout and restores this one afterwards.
 */
#define DEFAULT_PUT_MEDIA_HTTP_TIMEOUT_MS (10 * 1000)
#define DEFAULT_PUT_MEDIA_STACK_SIZE      (6 * 1024)
/*
 * Upload queue between the muxer callback and put_media_task.
 * The muxer emits two buffers per frame (header + block)
 */
#define DEFAULT_PUT_MEDIA_QUEUE_DEPTH     (CONFIG_PUT_MEDIA_UPLOAD_QUEUE_DEPTH)
/* How long put_media_enqueue() waits for room before giving up on a buffer. */
#define DEFAULT_PUT_MEDIA_ENQUEUE_TIMEOUT_MS (10)
/* How long put_media_deinit() waits for the task to leave its loop before giving up. */
#define PUT_MEDIA_TASK_EXIT_TIMEOUT_MS    (5000)
#define PUT_MEDIA_TASK_EXIT_POLL_MS       (10)

/* Put media session limit for AWS */
#define PUT_MEDIA_MAX_SESSION_MS          (40 * 60 * 1000)
/*
 * Margin subtracted from the credential expiry. It has to cover the wait for the
 * next cluster boundary (one GOP) plus the reconnect itself.
 */
#define PUT_MEDIA_CREDENTIAL_GRACE_MS     (30 * 1000)

#define PUT_MEDIA_MIN_SESSION_MS          (10 * 1000)

#define TAG "PUT_MEDIA"

static struct {
    char *pHost;
    char *pRegion;
    char *pService;
    char *pStreamName;
    char *pDataEndpoint;

    /* AWS access key, access secret and session token */
    char *pAwsAccessKeyId;
    char *pAwsSecretAccessKey;
    char *pAwsSessionToken;

    /* Iot certificates */
    char *pIotCredentialHost;
    char *pIotRoleAlias;
    char *pIotThingName;
    char *pIotX509RootCa;
    char *pIotX509Certificate;
    char *pIotX509PrivateKey;
    IotCredentialToken_t *pToken;

    /* Restful request parameters */
    KvsServiceParameter_t xServicePara;
    KvsDescribeStreamParameter_t xDescPara;
    KvsCreateStreamParameter_t xCreatePara;
    KvsGetDataEndpointParameter_t xGetDataEpPara;
    KvsPutMediaParameter_t xPutMediaPara;

    bool isEbmlHeaderUpdated;
    unsigned int uDataRetentionInHours;

    /* Resolved from put_media_config_t at init, falling back to the defaults. */
    unsigned int uEnqueueTimeoutMs;

    /* A cluster has to reach KVS whole. Whenever part of one is lost - a buffer
     * dropped because the upload queue was full, or everything still in flight
     * when the session was torn down - the rest of that cluster must not be
     * sent */
    volatile bool bResyncPending;

    /* Producer-side twin of bResyncPending, owned by put_media_enqueue().
     *
     * bResyncPending is cleared when the upload task *dequeues* a CLUSTER_HEADER,
     * which can be seconds after that header was queued. Gating the producer on
     * it would therefore keep discarding the very blocks belonging to the cluster
     * we just admitted. This flag is cleared as soon as a CLUSTER_HEADER is
     * enqueued, so a cluster is admitted or refused as a whole.
     *
     * The point is to refuse early: a buffer discarded here costs nothing,
     * whereas one discarded downstream has already cost a PSRAM copy and a queue
     * slot that a sendable buffer could have used. */
    volatile bool bEnqueueResync;

    /* Monotonic esp_timer deadline past which the session has to be reopened
     * with fresh credentials. INT64_MAX while nothing is scheduled. */
    int64_t xSessionDeadlineUs;

    /* Epoch of the stream's timecode zero, pinned when the first session opens
     * and reused by every reopen. Cluster timecodes are relative to it, so
     * re-stamping it on a reconnect would shift the whole timeline forward. */
    uint64_t uProducerStartTimestampMs;

    /* NULL whenever there is no open PutMedia request - including the window
     * inside put_media_session_restart(). Deliberately not volatile-qualified:
     * Kvs_putMediaStart() takes its address as a plain out-parameter, and
     * qualifying the field discards the qualifier there. put_media_is_session_live()
     * forces a fresh load with a cast instead. */
    PutMediaHandle xPutMediaHandle;
    get_mkv_ebml_hdr_func get_mkv_ebml_hdr_func;
    kvs_fetch_credentials_cb_t kvs_fetch_credentials_cb;
    put_media_event_handler_t event_handler;
    void *event_handler_user_ctx;
    TaskHandle_t  put_media_task_handle;
    uint8_t *put_media_task_stack;
    uint8_t *put_media_task_buffer;
    QueueHandle_t mkv_buf_queue;
    /* Set by the task just before it deletes itself, so deinit knows its stack is idle. */
    volatile bool task_exited;

    /* Written by put_media_task, read from caller tasks - put_media_enqueue(),
     * put_media_stop(), put_media_is_started() and put_media_is_session_live().
     * The last of those is on a per-frame path (the application's capture gate)
     * and exists to observe the transition promptly, so the read must not be
     * hoisted out of the caller's loop. */
    volatile enum {
        TASK_DEINITIATED = 0,
        TASK_INITIATED,
        TASK_STARTED,
        TASK_STOPPED,
    } state;
} put_media_priv_data = { 0 };

enum {
    START_TASK_SIGNAL = BIT1,
    STOP_TASK_SIGNAL  = BIT2,
    EXIT_TASK_SIGNAL  = BIT3
};

static put_media_err_t put_media_update(mkv_buf_info_t mkv_buf_info);
static void put_media_task(void *pvParameters);
static put_media_err_t put_media_session_restart(void);

static void put_media_notify_event(const put_media_event_data_t *event_data)
{
    if (put_media_priv_data.event_handler != NULL && event_data != NULL) {
        put_media_priv_data.event_handler(event_data, put_media_priv_data.event_handler_user_ctx);
    }
}

static bool isIotCertAvailable()
{
    if (put_media_priv_data.pIotCredentialHost != NULL && put_media_priv_data.pIotRoleAlias != NULL && put_media_priv_data.pIotThingName != NULL && put_media_priv_data.pIotX509RootCa != NULL && put_media_priv_data.pIotX509Certificate != NULL &&
            put_media_priv_data.pIotX509PrivateKey != NULL) {
        return true;
    } else {
        return false;
    }
}

static void updateIotCredential()
{
    IotCredentialToken_t *pToken = NULL;
    IotCredentialRequest_t xIotCredentialReq = {
        .pCredentialHost = put_media_priv_data.pIotCredentialHost,
        .pRoleAlias = put_media_priv_data.pIotRoleAlias,
        .pThingName = put_media_priv_data.pIotThingName,
        .pRootCA = put_media_priv_data.pIotX509RootCa,
        .pCertificate = put_media_priv_data.pIotX509Certificate,
        .pPrivateKey = put_media_priv_data.pIotX509PrivateKey
    };

    if (isIotCertAvailable()) {
        Iot_credentialTerminate(put_media_priv_data.pToken);
        put_media_priv_data.pToken = NULL;

        if ((pToken = Iot_getCredential(&xIotCredentialReq)) == NULL) {
            ESP_LOGE(TAG, "Failed to get Iot credential");
        } else {
            put_media_priv_data.pToken = pToken;
        }
    }
}

static int updateAndVerifyRestfulReqParameters()
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;

    put_media_priv_data.xServicePara.pcHost = put_media_priv_data.pHost;
    put_media_priv_data.xServicePara.pcRegion = put_media_priv_data.pRegion;
    put_media_priv_data.xServicePara.pcService = put_media_priv_data.pService;
    put_media_priv_data.xServicePara.uRecvTimeoutMs = DEFAULT_CONNECTION_TIMEOUT_MS;
    put_media_priv_data.xServicePara.uSendTimeoutMs = DEFAULT_CONNECTION_TIMEOUT_MS;

    if (put_media_priv_data.pToken != NULL) {
        put_media_priv_data.xServicePara.pcAccessKey = put_media_priv_data.pToken->pAccessKeyId;
        put_media_priv_data.xServicePara.pcSecretKey = put_media_priv_data.pToken->pSecretAccessKey;
        put_media_priv_data.xServicePara.pcToken = put_media_priv_data.pToken->pSessionToken;
    } else {
        if (put_media_priv_data.pAwsAccessKeyId == NULL || put_media_priv_data.pAwsSecretAccessKey == NULL) {
            res = PUT_MEDIA_ERROR_NO_AWS_ACCESS_KEY_OR_SECRET_KEY;
            ESP_LOGE(TAG, "No available aws access key");
        } else {
            put_media_priv_data.xServicePara.pcAccessKey = put_media_priv_data.pAwsAccessKeyId;
            put_media_priv_data.xServicePara.pcSecretKey = put_media_priv_data.pAwsSecretAccessKey;
            put_media_priv_data.xServicePara.pcToken = put_media_priv_data.pAwsSessionToken;
        }
    }

    if (res == PUT_MEDIA_ERRNO_NONE) {
        put_media_priv_data.xDescPara.pcStreamName = put_media_priv_data.pStreamName;

        put_media_priv_data.xCreatePara.pcStreamName = put_media_priv_data.pStreamName;
        put_media_priv_data.xCreatePara.uDataRetentionInHours = put_media_priv_data.uDataRetentionInHours;

        put_media_priv_data.xGetDataEpPara.pcStreamName = put_media_priv_data.pStreamName;

        put_media_priv_data.xPutMediaPara.pcStreamName = put_media_priv_data.pStreamName;
        /* The MKV mux emits cluster timecodes relative to stream start (not
         * absolute epoch), while x-amzn-producer-start-timestamp carries the
         * real epoch. Tell KVS the timecodes are RELATIVE so it computes the
         * producer timestamp as start + offset; ABSOLUTE made every fragment
         * land at ~1970 and disappear from producer-timestamp queries. */
        put_media_priv_data.xPutMediaPara.xTimecodeType = TIMECODE_TYPE_RELATIVE;
        /* Pin the epoch the timecodes are relative to on the first session and
         * keep it for every reopen. Left at 0 the SDK stamps "now" each time
         * Kvs_putMediaStart runs, which would push every fragment after a
         * reconnect forward by however long the previous session lasted. */
        if (put_media_priv_data.uProducerStartTimestampMs == 0) {
            put_media_priv_data.uProducerStartTimestampMs = getEpochTimestampInMs();
        }
        put_media_priv_data.xPutMediaPara.uProducerStartTimestampMs = put_media_priv_data.uProducerStartTimestampMs;
        put_media_priv_data.xPutMediaPara.uRecvTimeoutMs = DEFAULT_PUT_MEDIA_HTTP_TIMEOUT_MS;
        put_media_priv_data.xPutMediaPara.uSendTimeoutMs = DEFAULT_PUT_MEDIA_HTTP_TIMEOUT_MS;
    }

    return res;
}

static int setupDataEndpoint()
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;
    unsigned int uHttpStatusCode = 0;

    if (put_media_priv_data.xServicePara.pcPutMediaEndpoint != NULL) {
        /* Since we already have the endpoint, we needn't update it again. */
    } else {
        ESP_LOGI(TAG, "Try to describe stream");
        if ((res = Kvs_describeStream(&(put_media_priv_data.xServicePara), &(put_media_priv_data.xDescPara), &uHttpStatusCode)) != PUT_MEDIA_ERRNO_NONE) {
            ESP_LOGE(TAG, "Unable to describe stream");
            /* Propagate the res error */
        } else if (uHttpStatusCode != 200) {
            ESP_LOGI(TAG, "Failed to describe stream, status code:%u", uHttpStatusCode);
            res = PUT_MEDIA_GENERATE_RESTFUL_ERROR(uHttpStatusCode);

            ESP_LOGI(TAG, "Try to create stream");
            if ((res = Kvs_createStream(&(put_media_priv_data.xServicePara), &(put_media_priv_data.xCreatePara), &uHttpStatusCode)) != PUT_MEDIA_ERRNO_NONE) {
                ESP_LOGE(TAG, "Unable to create stream");
                /* Propagate the res error */
            } else if (uHttpStatusCode != 200) {
                ESP_LOGI(TAG, "Failed to create stream, status code:%u", uHttpStatusCode);
                res = PUT_MEDIA_GENERATE_RESTFUL_ERROR(uHttpStatusCode);
            }
        }

        if (res == PUT_MEDIA_ERRNO_NONE) {
            if ((res = Kvs_getDataEndpoint(&(put_media_priv_data.xServicePara), &(put_media_priv_data.xGetDataEpPara), &uHttpStatusCode, &(put_media_priv_data.pDataEndpoint))) != PUT_MEDIA_ERRNO_NONE) {
                ESP_LOGE(TAG, "Unable to get data endpoint");
                /* Propagate the res error */
            } else if (uHttpStatusCode != 200) {
                ESP_LOGI(TAG, "Failed to get data endpoint, status code:%u", uHttpStatusCode);
                res = PUT_MEDIA_GENERATE_RESTFUL_ERROR(uHttpStatusCode);
            } else {
                put_media_priv_data.xServicePara.pcPutMediaEndpoint = put_media_priv_data.pDataEndpoint;
            }
        }
    }

    if (res == PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGI(TAG, "PUT MEDIA endpoint: %s", put_media_priv_data.xServicePara.pcPutMediaEndpoint);
    }

    return res;
}

static int prvMallocAndStrcpyHelper(char **destination, const char *source)
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;

    if ((destination == NULL) || (source == NULL)) {
        /*If strDestination or strSource is a NULL pointer[...]these functions return EINVAL */
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        if (*destination != NULL) {
            // kvsFree(*destination);
            free(*destination);
            *destination = NULL;
        }
        if (mallocAndStrcpy_s(destination, source) != 0) {
            res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
        }
    }

    return res;
}

/* Aggregated so a sustained overrun costs one line a second rather than one per
 * buffer - at 20 fps the muxer emits ~40 buffers/s, and logging each drop is
 * slow enough to make the overrun worse. */
static uint32_t uDroppedBufs;
static uint32_t uDroppedBytes;
static uint32_t uSkippedBufs;

/*
 * Per-stage wall clock for one reporting window, so the pipeline can be told
 * apart from the network. All of it is accumulated by put_media_task except
 * uEnqueueUs, which the muxer task adds to; a torn read only skews one report.
 */
static struct {
    uint64_t uEnqueueUs;    /* producer: PSRAM copy + xQueueSend */
    uint64_t uDequeueWaitUs;/* task idle, waiting for a buffer - headroom, not cost */
    uint64_t uUpdateUs;     /* put_media_update() end to end */
    uint64_t uSendUs;       /* the Kvs_putMediaUpdate*() calls inside it */
    uint64_t uMaintainUs;   /* put_media_maintain() end to end */
    uint64_t uDoWorkUs;     /* Kvs_putMediaDoWork() inside it, i.e. the ack read */
    uint32_t uSentBufs;
    uint64_t uSentBytes;
} xStageMetrics;

static void prvReportDrop(size_t uDataLen, bool bSkippedEarly)
{
    static int64_t xLastReport;

    if (bSkippedEarly) {
        uSkippedBufs++;
    } else {
        uDroppedBufs++;
        uDroppedBytes += uDataLen;
    }

    int64_t xNow = esp_timer_get_time();
    if (xNow - xLastReport >= 1000000) {
        if (uDroppedBufs > 0 || uSkippedBufs > 0) {
            ESP_LOGW(TAG, "upload overrun: %" PRIu32 " buffers / %" PRIu32 " bytes lost, %" PRIu32 " more skipped to the next cluster", uDroppedBufs, uDroppedBytes, uSkippedBufs);
        }
        uDroppedBufs = 0;
        uDroppedBytes = 0;
        uSkippedBufs = 0;
        xLastReport = xNow;
    }
}

put_media_err_t put_media_enqueue(mkv_buf_info_t mkv_buf_info)
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;
    mkv_buf_info_t frame_copy = mkv_buf_info;
    int64_t xEnqueueStart = esp_timer_get_time();

    if (put_media_priv_data.state != TASK_STARTED) {
        ESP_LOGW(TAG, "Put media not started");
        res = PUT_MEDIA_ERROR_INVALID_STATE;
    } else if (put_media_priv_data.xPutMediaHandle == NULL) {
        /* The session is being reopened. Nothing enqueued now can go out: the
         * connection it would be written to does not exist yet, and once it does,
         * resync has to begin at a cluster boundary regardless.
         *
         * This must sit ahead of the bEnqueueResync branch below, and must not
         * fall through to it. put_media_service_stop() sets bEnqueueResync, but
         * that branch *clears* it as soon as one CLUSTER_HEADER is admitted - and
         * admitting a header while the handle is still NULL is what let an entire
         * reopen window into the queue: ~5 s at ~60 buffers/s, each holding a
         * PSRAM copy, none of them sendable. */
        res = PUT_MEDIA_ERROR_AWAITING_RESYNC;
        prvReportDrop(mkv_buf_info.uDataLen, true);
    } else if (put_media_priv_data.bEnqueueResync && mkv_buf_info.type != CLUSTER_HEADER) {
        /* Mid-cluster after a loss: this buffer can never be sent, so refuse it
         * before spending a PSRAM copy and a queue slot on it. Both are scarce
         * exactly when this path is hot. */
        res = PUT_MEDIA_ERROR_AWAITING_RESYNC;
        prvReportDrop(mkv_buf_info.uDataLen, true);
    } else if (!(frame_copy.buffer = heap_caps_malloc_prefer(frame_copy.uDataLen, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL))) {
        ESP_LOGE(TAG, "No memory to enqueue mkv buffer of size %d", frame_copy.uDataLen);
        res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
        /* This buffer is gone, so whatever cluster it belonged to is incomplete. */
        put_media_priv_data.bResyncPending = true;
        put_media_priv_data.bEnqueueResync = true;
        prvReportDrop(mkv_buf_info.uDataLen, false);
    } else {
        memcpy(frame_copy.buffer, mkv_buf_info.buffer, mkv_buf_info.uDataLen);

        if (xQueueSend(put_media_priv_data.mkv_buf_queue, &frame_copy, pdMS_TO_TICKS(put_media_priv_data.uEnqueueTimeoutMs)) != pdPASS) {
            free(frame_copy.buffer);
            res = PUT_MEDIA_ERROR_TIMEOUT;
            /* Upload is slower than the muxer. Same story: the cluster this
             * belonged to can no longer be sent intact, so resync at the next one
             * rather than uploading a fragment KVS will reject. */
            put_media_priv_data.bResyncPending = true;
            put_media_priv_data.bEnqueueResync = true;
            prvReportDrop(mkv_buf_info.uDataLen, false);
        } else if (mkv_buf_info.type == CLUSTER_HEADER) {
            /* A whole cluster is now admissible again. */
            put_media_priv_data.bEnqueueResync = false;
        }
    }

    xStageMetrics.uEnqueueUs += (uint64_t)(esp_timer_get_time() - xEnqueueStart);

    return res;
}

put_media_err_t put_media_start()
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;

    if (put_media_priv_data.state == TASK_STARTED) {
        ESP_LOGW(TAG, "Put media already started");
        res = PUT_MEDIA_ERRNO_NONE;
    } else if (put_media_priv_data.state == TASK_DEINITIATED) {
        ESP_LOGE(TAG, "Initialize Put media before starting");
        res = PUT_MEDIA_ERROR_INVALID_STATE;
    } else {
        xTaskNotify(put_media_priv_data.put_media_task_handle, START_TASK_SIGNAL, eSetValueWithOverwrite);
    }

    return res;
}

bool put_media_is_started()
{
    return put_media_priv_data.state == TASK_STARTED;
}

bool put_media_is_session_live(void)
{
    /* Read the handle through a volatile lvalue. It is written by put_media_task
     * while this runs on the caller's per-frame path, and the point of the gate is
     * to see the transition promptly, so the load must not be hoisted out of the
     * caller's loop. The field itself cannot carry the qualifier - see its
     * declaration. */
    PutMediaHandle xHandle = *(PutMediaHandle volatile *) &put_media_priv_data.xPutMediaHandle;

    return put_media_priv_data.state == TASK_STARTED && xHandle != NULL;
}

unsigned int put_media_get_pending_count(void)
{
    if (put_media_priv_data.mkv_buf_queue == NULL) {
        return 0;
    }

    return (unsigned int) uxQueueMessagesWaiting(put_media_priv_data.mkv_buf_queue);
}

put_media_err_t put_media_stop()
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;

    if (put_media_priv_data.put_media_task_handle && put_media_priv_data.state == TASK_STARTED) {
        xTaskNotify(put_media_priv_data.put_media_task_handle, STOP_TASK_SIGNAL, eSetValueWithOverwrite);
        while (put_media_priv_data.state == TASK_STARTED) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return res;
}

put_media_err_t put_media_init(put_media_config_t put_media_config)
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;

    /* Both are optional; zero selects the default so existing callers, which use
     * designated initialisers and never mention these fields, are unaffected. */
    unsigned int uQueueDepth = (put_media_config.uUploadQueueDepth > 0) ? put_media_config.uUploadQueueDepth : DEFAULT_PUT_MEDIA_QUEUE_DEPTH;
    put_media_priv_data.uEnqueueTimeoutMs =
        (put_media_config.uEnqueueTimeoutMs > 0) ? put_media_config.uEnqueueTimeoutMs : DEFAULT_PUT_MEDIA_ENQUEUE_TIMEOUT_MS;

    if (put_media_config.pcHost == NULL || put_media_config.pcRegion == NULL || put_media_config.pcService == NULL ||
            put_media_config.pcStreamName == NULL || put_media_config.get_mkv_ebml_hdr_func == NULL || put_media_config.kvs_fetch_credentials_cb == NULL || put_media_config.uDataRetentionHours == 0) {
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        ESP_LOGE(TAG, "Invalid parameter");
    } else if (
        (res = prvMallocAndStrcpyHelper(&(put_media_priv_data.pHost), put_media_config.pcHost)) != PUT_MEDIA_ERRNO_NONE ||
        (res = prvMallocAndStrcpyHelper(&(put_media_priv_data.pRegion), put_media_config.pcRegion)) != PUT_MEDIA_ERRNO_NONE ||
        (res = prvMallocAndStrcpyHelper(&(put_media_priv_data.pService), put_media_config.pcService)) != PUT_MEDIA_ERRNO_NONE ||
        (res = prvMallocAndStrcpyHelper(&(put_media_priv_data.pStreamName), put_media_config.pcStreamName)) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "OOM: parameters");
        /* Propagate the res error */
    } else if (!(put_media_priv_data.mkv_buf_queue = xQueueCreate(uQueueDepth, sizeof(mkv_buf_info_t)))) {
        ESP_LOGE(TAG, "Couldn't create queue for put media");
        res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
    } else if (!(put_media_priv_data.put_media_task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL)) ||
               !(put_media_priv_data.put_media_task_stack = heap_caps_malloc_prefer(DEFAULT_PUT_MEDIA_STACK_SIZE, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL))) {
        ESP_LOGE(TAG, "No memory to allocate task");
        res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
    } else if (!(put_media_priv_data.put_media_task_handle = xTaskCreateStatic(&put_media_task, "put_media_task", DEFAULT_PUT_MEDIA_STACK_SIZE,
                                                                               NULL, 10, put_media_priv_data.put_media_task_stack, (StaticTask_t*) put_media_priv_data.put_media_task_buffer))) {
        ESP_LOGE(TAG, "Couldn't create put media task");
        res = PUT_MEDIA_ERROR_GENERIC;
    } else {
        put_media_priv_data.pDataEndpoint = NULL;
        put_media_priv_data.pAwsAccessKeyId = NULL;
        put_media_priv_data.pAwsSecretAccessKey = NULL;
        put_media_priv_data.pIotCredentialHost = NULL;
        put_media_priv_data.pIotRoleAlias = NULL;
        put_media_priv_data.pIotThingName = NULL;
        put_media_priv_data.pIotX509RootCa = NULL;
        put_media_priv_data.pIotX509Certificate = NULL;
        put_media_priv_data.pIotX509PrivateKey = NULL;
        put_media_priv_data.pToken = NULL;
        put_media_priv_data.isEbmlHeaderUpdated = false;
        put_media_priv_data.bResyncPending = true;
        put_media_priv_data.bEnqueueResync = true;

        put_media_priv_data.uDataRetentionInHours = put_media_config.uDataRetentionHours;

        put_media_priv_data.xSessionDeadlineUs = INT64_MAX;
        put_media_priv_data.uProducerStartTimestampMs = 0;

        put_media_priv_data.xPutMediaHandle = NULL;

        put_media_priv_data.get_mkv_ebml_hdr_func = put_media_config.get_mkv_ebml_hdr_func;
        put_media_priv_data.kvs_fetch_credentials_cb = put_media_config.kvs_fetch_credentials_cb;
        put_media_priv_data.event_handler = put_media_config.event_handler;
        put_media_priv_data.event_handler_user_ctx = put_media_config.event_handler_user_ctx;
    }

    if (res == PUT_MEDIA_ERRNO_NONE) {
        put_media_priv_data.state = TASK_INITIATED;
    }

    return res;
}

static put_media_err_t put_media_update(mkv_buf_info_t mkv_buf_info)
{
    static mkv_buf_info_t mkv_header = {.buffer = NULL, .uDataLen = 0};

    if (!put_media_priv_data.xPutMediaHandle) {
        return PUT_MEDIA_ERROR_INVALID_STATE;
    }

    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;

    switch (mkv_buf_info.type) {
    case SEGMENT_HEADER: {
        break;
    }

    case CLUSTER_HEADER:
    case BLOCK_HEADER: {
        if (mkv_buf_info.type == CLUSTER_HEADER && esp_timer_get_time() >= put_media_priv_data.xSessionDeadlineUs) {
            ESP_LOGI(TAG, "Session deadline reached, reopening at cluster boundary");
            if (put_media_session_restart() != PUT_MEDIA_ERRNO_NONE) {
                ESP_LOGE(TAG, "Reopen failed, retrying at the next cluster");
                break;
            }
        }

        if (mkv_buf_info.type == CLUSTER_HEADER && put_media_priv_data.bResyncPending) {
            ESP_LOGI(TAG, "Resynced at cluster boundary");
            put_media_priv_data.bResyncPending = false;
        }

        if (mkv_header.buffer != NULL) {
            free(mkv_header.buffer);
            mkv_header.buffer = NULL;
            mkv_header.uDataLen = 0;
        }

        mkv_header = mkv_buf_info;
        mkv_header.buffer = heap_caps_malloc(mkv_buf_info.uDataLen, MALLOC_CAP_SPIRAM);
        if (!mkv_header.buffer) {
            res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
            break;
        }

        memcpy(mkv_header.buffer, mkv_buf_info.buffer, mkv_header.uDataLen);
        break;
    }

    case CLUSTER_BLOCK:
    case SIMPLE_BLOCK: {
        if (!(put_media_priv_data.xPutMediaHandle)) {
            // ESP_LOGE(TAG, "Put media is not started yet!");
            res = PUT_MEDIA_ERROR_INVALID_STATE;
        } else if (put_media_priv_data.bResyncPending) {
            /* Waiting for the next cluster. Not an error.
             * Discard the block and keep going. */
            break;
        } else if (!(put_media_priv_data.isEbmlHeaderUpdated) && mkv_header.type != CLUSTER_HEADER) {
            /* Nothing has been sent on this session yet and the cached header is
             * mid-cluster, so there is no valid place to start. Wait for a
             * cluster rather than failing. */
            put_media_priv_data.bResyncPending = true;
            break;
        } else {
            if (!(put_media_priv_data.isEbmlHeaderUpdated)) {
                mkv_buf_info_t mkv_ebml_buf;
                res = put_media_priv_data.get_mkv_ebml_hdr_func(&mkv_ebml_buf);
                if (res != PUT_MEDIA_ERRNO_NONE) {
                    break;
                }
                ESP_LOGI(TAG, "Sending ebml_header of size: %d", mkv_ebml_buf.uDataLen);
                int64_t xSendStart = esp_timer_get_time();
                res = Kvs_putMediaUpdateRaw(put_media_priv_data.xPutMediaHandle, mkv_ebml_buf.buffer, mkv_ebml_buf.uDataLen);
                xStageMetrics.uSendUs += (uint64_t)(esp_timer_get_time() - xSendStart);
                free(mkv_ebml_buf.buffer);
                if (res != PUT_MEDIA_ERRNO_NONE) {
                    break;
                }

                put_media_priv_data.isEbmlHeaderUpdated = true;
            }

            int64_t xSendStart = esp_timer_get_time();
            res = Kvs_putMediaUpdate(put_media_priv_data.xPutMediaHandle, mkv_header.buffer, mkv_header.uDataLen, mkv_buf_info.buffer, mkv_buf_info.uDataLen);
            xStageMetrics.uSendUs += (uint64_t)(esp_timer_get_time() - xSendStart);
            if (res == PUT_MEDIA_ERRNO_NONE) {
                xStageMetrics.uSentBufs++;
                xStageMetrics.uSentBytes += mkv_header.uDataLen + mkv_buf_info.uDataLen;
            }
        }

        break;
    }
    }
    return res;
}

/*
 * Turn the expiry the provider reported into the monotonic instant at which this
 * session has to be reopened. Called once the connection is actually up, so the
 * clock starts from the same point AWS ages the connection from.
 */
static void put_media_arm_session_deadline(uint32_t uExpirationSec)
{
    /* UINT32_MAX is the "does not expire" sentinel - static keys use it. The AWS
     * ceiling then governs on its own. Widen before scaling either way: seconds
     * to microseconds overflows 32 bits well before the sentinel. */
    int64_t xCredentialUs = (uExpirationSec == UINT32_MAX)
                            ? INT64_MAX
                            : ((int64_t) uExpirationSec * 1000 - PUT_MEDIA_CREDENTIAL_GRACE_MS) * 1000;
    int64_t xCeilingUs = (int64_t) PUT_MEDIA_MAX_SESSION_MS * 1000;
    int64_t xLifetimeUs = (xCredentialUs < xCeilingUs) ? xCredentialUs : xCeilingUs;

    if (xLifetimeUs < (int64_t) PUT_MEDIA_MIN_SESSION_MS * 1000) {
        ESP_LOGW(TAG, "credential expires in %" PRIu32 "s, shorter than the %d ms refresh grace; clamping session to %d ms",
                 uExpirationSec, PUT_MEDIA_CREDENTIAL_GRACE_MS, PUT_MEDIA_MIN_SESSION_MS);
        xLifetimeUs = (int64_t) PUT_MEDIA_MIN_SESSION_MS * 1000;
    }

    put_media_priv_data.xSessionDeadlineUs = esp_timer_get_time() + xLifetimeUs;
    ESP_LOGI(TAG, "Session opened, reopening in %" PRId64 " s", xLifetimeUs / 1000000);
}

static put_media_err_t put_media_service_start()
{
    unsigned int uHttpStatusCode = 0;
    uint32_t expiration = 0;
    uint32_t uAccessKeyLen = 0;
    uint32_t uSecretKeyLen = 0;
    uint32_t uSessionTokenLen = 0;
    put_media_err_t res = put_media_priv_data.kvs_fetch_credentials_cb(0,
                                                                       (const char **) &put_media_priv_data.pAwsAccessKeyId,
                                                                       &uAccessKeyLen,
                                                                       (const char **) &put_media_priv_data.pAwsSecretAccessKey,
                                                                       &uSecretKeyLen,
                                                                       (const char **) &put_media_priv_data.pAwsSessionToken,
                                                                       &uSessionTokenLen,
                                                                       &expiration);

    if (res != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Could not fetch credentials");
    } else if ((res = updateAndVerifyRestfulReqParameters()) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to setup KVS");
    } else if ((res = setupDataEndpoint()) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to setup data endpoint");
    } else if ((res = Kvs_putMediaStart(&(put_media_priv_data.xServicePara), &(put_media_priv_data.xPutMediaPara), &uHttpStatusCode, &(put_media_priv_data.xPutMediaHandle))) != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Failed to setup PUT MEDIA");
    } else if (uHttpStatusCode != 200 && uHttpStatusCode != 100) {
        res = PUT_MEDIA_GENERATE_RESTFUL_ERROR(uHttpStatusCode);
        ESP_LOGE(TAG, "PUT MEDIA http status code:%d", uHttpStatusCode);
    } else {
        put_media_arm_session_deadline(expiration);
    }

    return res;
}

static void put_media_service_stop()
{
    if (put_media_priv_data.xPutMediaHandle != NULL) {
        Kvs_putMediaFinish(put_media_priv_data.xPutMediaHandle);
        put_media_priv_data.xPutMediaHandle = NULL;
        put_media_priv_data.isEbmlHeaderUpdated = false;
        /* Anything that was in flight is gone with the connection, so the next
         * session has to begin at a cluster boundary. */
        put_media_priv_data.bResyncPending = true;
        put_media_priv_data.bEnqueueResync = true;
    }
}

/*
 * Drop the current PutMedia session and open a fresh one. Used both when a
 * fragment ack reports an error and when a chunk write fails - the latter
 * leaves the HTTP request body mid-chunk, so the connection is unusable and
 * continuing to write into it would corrupt the stream.
 */
static put_media_err_t put_media_session_restart(void)
{
    ESP_LOGI(TAG, "Restarting Put Media session");
    put_media_service_stop();
    return put_media_service_start();
}

put_media_err_t put_media_maintain()
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;
    if (!put_media_priv_data.xPutMediaHandle) {
        return PUT_MEDIA_ERROR_INVALID_STATE;
    } else if (!(put_media_priv_data.xPutMediaHandle)) {
        // ESP_LOGW(TAG, "Put media not initialized yet");
        res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
    } else {
        ePutMediaFragmentAckEventType eAckEventType;
        uint64_t puFragmentTimecode;
        unsigned int puErrorId;

        while (Kvs_putMediaReadFragmentAck(put_media_priv_data.xPutMediaHandle, &eAckEventType, &puFragmentTimecode, &puErrorId) == 0) {
            put_media_event_data_t ev = {
                .event = PUT_MEDIA_ON_ACK_RECV,
                .data = { .ack = { .eventType = eAckEventType, .uFragmentTimecode = puFragmentTimecode, .uErrorId = puErrorId } }
            };
            put_media_notify_event(&ev);
            if (eAckEventType == ePersisted) {
                ESP_LOGW(TAG, "key-frame with timecode %" PRIu64 " is persisted", puFragmentTimecode);
            } else if (eAckEventType == eError) {
                /* Please refer to the following link to get more information on the error ID.
                 *      https://docs.aws.amazon.com/kinesisvideostreams/latest/dg/API_dataplane_PutMedia.html
                 */
                ESP_LOGE(TAG, "key-frame with timecode %" PRIu64 " has error: %d", puFragmentTimecode, puErrorId);
                /* KVS rejected the fragment - 4006 means it did not parse as
                 * MKV, which is what an incomplete cluster looks like from the
                 * far end. Start clean at the next one. */
                put_media_priv_data.bResyncPending = true;
                put_media_priv_data.bEnqueueResync = true;
            }
        }

        int64_t xDoWorkStart = esp_timer_get_time();
        res = Kvs_putMediaDoWork(put_media_priv_data.xPutMediaHandle);
        xStageMetrics.uDoWorkUs += (uint64_t)(esp_timer_get_time() - xDoWorkStart);
        if (res != PUT_MEDIA_ERRNO_NONE) {
            while (Kvs_putMediaReadFragmentAck(put_media_priv_data.xPutMediaHandle, &eAckEventType, &puFragmentTimecode, &puErrorId) == 0) {
                put_media_event_data_t ev = {
                    .event = PUT_MEDIA_ON_ACK_RECV,
                    .data = { .ack = { .eventType = eAckEventType, .uFragmentTimecode = puFragmentTimecode, .uErrorId = puErrorId } }
                };
                put_media_notify_event(&ev);
                if (eAckEventType == eError) {
                    /* Please refer to the following link to get more information on the error ID.
                     *      https://docs.aws.amazon.com/kinesisvideostreams/latest/dg/API_dataplane_PutMedia.html
                     */
                    ESP_LOGE(TAG, "key-frame with timecode %" PRIu64 " has error: %d", puFragmentTimecode, puErrorId);
                    /* KVS rejected the fragment - 4006 means it did not parse as
                     * MKV, which is what an incomplete cluster looks like from
                     * the far end. Start clean at the next one. */
                    put_media_priv_data.bResyncPending = true;
                    put_media_priv_data.bEnqueueResync = true;
                }
            }

            res = put_media_session_restart();
        }
    }

    return res;
}

/*
 * One line per second accounting for where the upload thread's wall clock went.
 *
 * The window is the denominator: if send+dowork is most of it, the pipeline is
 * busy and the limit is the socket or the framing around it. If wait dominates,
 * the upload thread is idle and the limit is upstream. Anything unaccounted for
 * is time in the loop itself - notify waits, frees, the resync checks.
 */
static void prvReportStageMetrics(int64_t xWindowUs, size_t uTotalSent)
{
    uint64_t uWindowMs = (uint64_t)xWindowUs / 1000;
    if (uWindowMs == 0) {
        uWindowMs = 1;
    }

    ESP_LOGD(TAG,
             "stage/%llums: sent=%" PRIu32 " buf %llu KB (total %zu KB) | wait=%llums update=%llums (send=%llums) maintain=%llums (dowork=%llums) enqueue=%llums",
             (unsigned long long)uWindowMs, xStageMetrics.uSentBufs, (unsigned long long)(xStageMetrics.uSentBytes / 1024), uTotalSent / 1024,
             (unsigned long long)(xStageMetrics.uDequeueWaitUs / 1000), (unsigned long long)(xStageMetrics.uUpdateUs / 1000), (unsigned long long)(xStageMetrics.uSendUs / 1000),
             (unsigned long long)(xStageMetrics.uMaintainUs / 1000), (unsigned long long)(xStageMetrics.uDoWorkUs / 1000), (unsigned long long)(xStageMetrics.uEnqueueUs / 1000));

    memset(&xStageMetrics, 0, sizeof(xStageMetrics));
}

static void put_media_task(void *pvParameters)
{
    assert(put_media_priv_data.mkv_buf_queue);

    size_t data_sent = 0;
    int64_t prev_time = 0;

    bool exit_requested = false;

    while (!exit_requested) {
        uint32_t notification_value = 0;
        while (xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY) == pdTRUE
                && !(notification_value & (START_TASK_SIGNAL | EXIT_TASK_SIGNAL))) { }

        if (notification_value & EXIT_TASK_SIGNAL) {
            break;
        }

        put_media_err_t res = put_media_service_start();

        if (res != PUT_MEDIA_ERRNO_NONE) {
            ESP_LOGE(TAG, "Error while put media start %d", res);
            put_media_event_data_t ev = { .event = PUT_MEDIA_ON_ERROR, .data = { .error_code = res } };
            put_media_notify_event(&ev);
            continue;
        }

        put_media_priv_data.state = TASK_STARTED;
        ESP_LOGI(TAG, "Put media started");
        {
            put_media_event_data_t ev = { .event = PUT_MEDIA_ON_START, .data = { 0 } };
            put_media_notify_event(&ev);
        }

        notification_value = 0;

        while (true) {
            mkv_buf_info_t mkv_buf_info;
            xTaskNotifyWait(0, UINT32_MAX, &notification_value, 1);
            if (notification_value & EXIT_TASK_SIGNAL) {
                exit_requested = true;
                break;
            }
            if (notification_value & STOP_TASK_SIGNAL) {
                break;
            }

            int64_t xWaitStart = esp_timer_get_time();
            BaseType_t xGot = xQueueReceive(put_media_priv_data.mkv_buf_queue, &mkv_buf_info, pdMS_TO_TICKS(100));
            xStageMetrics.uDequeueWaitUs += (uint64_t)(esp_timer_get_time() - xWaitStart);

            if (xGot == pdTRUE) {
                put_media_err_t res;
                int64_t xUpdateStart = esp_timer_get_time();
                res = put_media_update(mkv_buf_info);
                xStageMetrics.uUpdateUs += (uint64_t)(esp_timer_get_time() - xUpdateStart);
                if (res != PUT_MEDIA_ERRNO_NONE) {
                    ESP_LOGE(TAG, "put media update failed: %d", res);
                    put_media_event_data_t ev = { .event = PUT_MEDIA_ON_ERROR, .data = { .error_code = res } };
                    put_media_notify_event(&ev);

                    /*
                     * The write may have failed partway through a chunk, which
                     * leaves the request body unparseable. Reconnect before the
                     * next frame instead of writing into a broken stream.
                     */
                    free(mkv_buf_info.buffer);
                    if ((res = put_media_session_restart()) != PUT_MEDIA_ERRNO_NONE) {
                        ESP_LOGE(TAG, "put media session restart failed: %d", res);
                        put_media_event_data_t restart_ev = { .event = PUT_MEDIA_ON_ERROR, .data = { .error_code = res } };
                        put_media_notify_event(&restart_ev);
                    }
                    continue;
                }
                data_sent += mkv_buf_info.uDataLen;
                int64_t xMaintainStart = esp_timer_get_time();
                res = put_media_maintain();
                xStageMetrics.uMaintainUs += (uint64_t)(esp_timer_get_time() - xMaintainStart);
                if (res != PUT_MEDIA_ERRNO_NONE) {
                    ESP_LOGE(TAG, "put media maintain failed: %d", res);
                    put_media_event_data_t ev = { .event = PUT_MEDIA_ON_ERROR, .data = { .error_code = res } };
                    put_media_notify_event(&ev);
                }
                free(mkv_buf_info.buffer);

                int64_t ctime = esp_timer_get_time();
                if (ctime - prev_time > 1000 * 1000 * 1) {
                    prvReportStageMetrics(ctime - prev_time, data_sent);
                    prev_time = ctime;
                }
            }
        }

        put_media_service_stop();

        put_media_priv_data.state = TASK_STOPPED;
        data_sent = 0;
        /* The muxer restarts with the stream, so the next start re-anchors the
         * timecode epoch. Only reopens within one stream keep the old value. */
        put_media_priv_data.uProducerStartTimestampMs = 0;
        put_media_priv_data.xSessionDeadlineUs = INT64_MAX;
        {
            put_media_event_data_t ev = { .event = PUT_MEDIA_ON_STOP, .data = { 0 } };
            put_media_notify_event(&ev);
        }

        ESP_LOGI(TAG, "Put media stopped");
    }

    /* Signal deinit that the stack is no longer in use, then retire. */
    put_media_priv_data.state = TASK_DEINITIATED;
    put_media_priv_data.task_exited = true;
    vTaskDelete(NULL);
}

/*
 * Ask the task to leave its loop and wait for it to actually get there.
 * Returns false if it did not, in which case its stack must not be freed.
 */
static bool put_media_task_shutdown()
{
    int waited_ms = 0;

    if (put_media_priv_data.put_media_task_handle == NULL) {
        return true;
    }

    put_media_priv_data.task_exited = false;
    xTaskNotify(put_media_priv_data.put_media_task_handle, EXIT_TASK_SIGNAL, eSetValueWithOverwrite);

    while (!put_media_priv_data.task_exited && waited_ms < PUT_MEDIA_TASK_EXIT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(PUT_MEDIA_TASK_EXIT_POLL_MS));
        waited_ms += PUT_MEDIA_TASK_EXIT_POLL_MS;
    }

    put_media_priv_data.put_media_task_handle = NULL;

    if (!put_media_priv_data.task_exited) {
        return false;
    }

    /* Give the idle task a chance to finish with the deleted task first. */
    vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

static void put_media_drain_queue()
{
    mkv_buf_info_t mkv_buf_info;

    if (put_media_priv_data.mkv_buf_queue == NULL) {
        return;
    }

    while (xQueueReceive(put_media_priv_data.mkv_buf_queue, &mkv_buf_info, 0) == pdTRUE) {
        free(mkv_buf_info.buffer);
    }
}

put_media_err_t put_media_deinit()
{
    put_media_err_t err = put_media_stop();
    bool task_stack_reclaimable;

    if (err != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Could not stop put media");
        return err;
    }

    /* Retire the task before touching anything it might still be using. */
    task_stack_reclaimable = put_media_task_shutdown();
    if (!task_stack_reclaimable) {
        ESP_LOGE(TAG, "put_media task did not exit in time; leaking its stack rather than freeing it in use");
    }

    if (put_media_priv_data.pHost != NULL) {
        free(put_media_priv_data.pHost);
        put_media_priv_data.pHost = NULL;
    }
    if (put_media_priv_data.pRegion != NULL) {
        free(put_media_priv_data.pRegion);
        put_media_priv_data.pRegion = NULL;
    }
    if (put_media_priv_data.pService != NULL) {
        free(put_media_priv_data.pService);
        put_media_priv_data.pService = NULL;
    }
    if (put_media_priv_data.pStreamName != NULL) {
        free(put_media_priv_data.pStreamName);
        put_media_priv_data.pStreamName = NULL;
    }
    if (put_media_priv_data.pDataEndpoint != NULL) {
        free(put_media_priv_data.pDataEndpoint);
        put_media_priv_data.pDataEndpoint = NULL;
    }
    /*
     * The AWS credential strings are NOT owned here: kvs_fetch_credentials_cb
     * writes its own pointers into these fields (see put_media_service_start),
     * and the callback keeps ownership. Only drop the references.
     */
    put_media_priv_data.pAwsAccessKeyId = NULL;
    put_media_priv_data.pAwsSecretAccessKey = NULL;
    put_media_priv_data.pAwsSessionToken = NULL;
    if (put_media_priv_data.pIotCredentialHost != NULL) {
        free(put_media_priv_data.pIotCredentialHost);
        put_media_priv_data.pIotCredentialHost = NULL;
    }
    if (put_media_priv_data.pIotRoleAlias != NULL) {
        free(put_media_priv_data.pIotRoleAlias);
        put_media_priv_data.pIotRoleAlias = NULL;
    }
    if (put_media_priv_data.pIotThingName != NULL) {
        free(put_media_priv_data.pIotThingName);
        put_media_priv_data.pIotThingName = NULL;
    }
    if (put_media_priv_data.pIotX509RootCa != NULL) {
        free(put_media_priv_data.pIotX509RootCa);
        put_media_priv_data.pIotX509RootCa = NULL;
    }
    if (put_media_priv_data.pIotX509Certificate != NULL) {
        free(put_media_priv_data.pIotX509Certificate);
        put_media_priv_data.pIotX509Certificate = NULL;
    }
    if (put_media_priv_data.pIotX509PrivateKey != NULL) {
        free(put_media_priv_data.pIotX509PrivateKey);
        put_media_priv_data.pIotX509PrivateKey = NULL;
    }

    if (task_stack_reclaimable) {
        if (put_media_priv_data.put_media_task_buffer) {
            free(put_media_priv_data.put_media_task_buffer);
            put_media_priv_data.put_media_task_buffer = NULL;
        }

        if (put_media_priv_data.put_media_task_stack) {
            free(put_media_priv_data.put_media_task_stack);
            put_media_priv_data.put_media_task_stack = NULL;
        }
    }

    if (put_media_priv_data.mkv_buf_queue) {
        /* Frames still queued own their buffers; release them before the queue goes. */
        put_media_drain_queue();
        vQueueDelete(put_media_priv_data.mkv_buf_queue);
        put_media_priv_data.mkv_buf_queue = NULL;
    }

    put_media_priv_data.state = TASK_DEINITIATED;
    return PUT_MEDIA_ERRNO_NONE;
}
