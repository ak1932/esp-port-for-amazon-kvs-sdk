#include "azure_c_shared_utility/crt_abstractions.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "kvs/iot_credential_provider.h"
#include "kvs/put_media_errors.h"
#include "kvs/restapi.h"
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
/* Longer timeouts for PutMedia stream to avoid transport_base poll timeout on slow/congested links */
#define DEFAULT_PUT_MEDIA_HTTP_TIMEOUT_MS (2000)
#define DEFAULT_PUT_MEDIA_STACK_SIZE      (6 * 1024)

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

    PutMediaHandle xPutMediaHandle;
    get_mkv_ebml_hdr_func get_mkv_ebml_hdr_func;
    kvs_fetch_credentials_cb_t kvs_fetch_credentials_cb;
    put_media_event_handler_t event_handler;
    void *event_handler_user_ctx;
    TaskHandle_t  put_media_task_handle;
    uint8_t *put_media_task_stack;
    uint8_t *put_media_task_buffer;
    QueueHandle_t mkv_buf_queue;

    enum {
        TASK_DEINITIATED = 0,
        TASK_INITIATED,
        TASK_STARTED,
        TASK_STOPPED,
    } state;
} put_media_priv_data = { 0 };

enum {
    START_TASK_SIGNAL = BIT1,
    STOP_TASK_SIGNAL  = BIT2
};

static put_media_err_t put_media_update(mkv_buf_info_t mkv_buf_info);
static void put_media_task(void *pvParameters);

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
        put_media_priv_data.xPutMediaPara.xTimecodeType = TIMECODE_TYPE_ABSOLUTE;
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

put_media_err_t put_media_enqueue(mkv_buf_info_t mkv_buf_info)
{
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;
    mkv_buf_info_t frame_copy = mkv_buf_info;

    if (put_media_priv_data.state != TASK_STARTED) {
        ESP_LOGD(TAG, "Put media not started");
        res = PUT_MEDIA_ERROR_INVALID_STATE;
    } else if (!(frame_copy.buffer = heap_caps_malloc_prefer(frame_copy.uDataLen, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL))) {
        ESP_LOGE(TAG, "No memory to enqueue mkv buffer of size %d", frame_copy.uDataLen);
        res = PUT_MEDIA_ERROR_OUT_OF_MEMORY;
    } else {
        memcpy(frame_copy.buffer, mkv_buf_info.buffer, mkv_buf_info.uDataLen);

        if (xQueueSend(put_media_priv_data.mkv_buf_queue, &frame_copy, pdMS_TO_TICKS(10)) != pdPASS) {
            free(frame_copy.buffer);
            res = PUT_MEDIA_ERROR_TIMEOUT;
        }
    }

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
    return put_media_priv_data.xPutMediaHandle != NULL;
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
    } else if (!(put_media_priv_data.mkv_buf_queue = xQueueCreate(50, sizeof(mkv_buf_info_t)))) {
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

        put_media_priv_data.uDataRetentionInHours = put_media_config.uDataRetentionHours;

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
        if (mkv_header.buffer != NULL) {
            free(mkv_header.buffer);
            mkv_header.buffer = NULL;
            mkv_header.uDataLen = 0;
        }

        mkv_header = mkv_buf_info;
        mkv_header.buffer = malloc(mkv_buf_info.uDataLen);
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
        } else if (!(put_media_priv_data.isEbmlHeaderUpdated) && mkv_header.type != CLUSTER_HEADER) {
            res = PUT_MEDIA_ERROR_INVALID_ARGUMENT;
        } else {
            if (!(put_media_priv_data.isEbmlHeaderUpdated)) {
                mkv_buf_info_t mkv_ebml_buf;
                res = put_media_priv_data.get_mkv_ebml_hdr_func(&mkv_ebml_buf);
                if (res != PUT_MEDIA_ERRNO_NONE) {
                    break;
                }
                ESP_LOGI(TAG, "Sending ebml_header of size: %d", mkv_ebml_buf.uDataLen);
                res = Kvs_putMediaUpdateRaw(put_media_priv_data.xPutMediaHandle, mkv_ebml_buf.buffer, mkv_ebml_buf.uDataLen);
                free(mkv_ebml_buf.buffer);
                if (res != PUT_MEDIA_ERRNO_NONE) {
                    break;
                }

                put_media_priv_data.isEbmlHeaderUpdated = true;
            }

            res = Kvs_putMediaUpdate(put_media_priv_data.xPutMediaHandle, mkv_header.buffer, mkv_header.uDataLen, mkv_buf_info.buffer, mkv_buf_info.uDataLen);
        }

        break;
    }
    }
    return res;
}

static put_media_err_t put_media_service_start()
{
    unsigned int uHttpStatusCode = 0;
    uint32_t expiration;
    uint32_t len;
    put_media_err_t res = PUT_MEDIA_ERRNO_NONE;
    put_media_priv_data.kvs_fetch_credentials_cb(0,
                                                 (const char **) &put_media_priv_data.pAwsAccessKeyId,
                                                 &len,
                                                 (const char **) &put_media_priv_data.pAwsSecretAccessKey,
                                                 &len,
                                                 (const char **) &put_media_priv_data.pAwsSessionToken,
                                                 &len,
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
    }

    return res;
}

static void put_media_service_stop()
{
    if (put_media_priv_data.xPutMediaHandle != NULL) {
        Kvs_putMediaFinish(put_media_priv_data.xPutMediaHandle);
        put_media_priv_data.xPutMediaHandle = NULL;
        put_media_priv_data.isEbmlHeaderUpdated = false;
    }
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
                ESP_LOGD(TAG, "key-frame with timecode %" PRIu64 " is persisted", puFragmentTimecode);
            } else if (eAckEventType == eError) {
                /* Please refer to the following link to get more information on the error ID.
                 *      https://docs.aws.amazon.com/kinesisvideostreams/latest/dg/API_dataplane_PutMedia.html
                 */
                ESP_LOGE(TAG, "key-frame with timecode %" PRIu64 " has error: %d", puFragmentTimecode, puErrorId);
            }
        }

        res = Kvs_putMediaDoWork(put_media_priv_data.xPutMediaHandle);
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
                }
            }

            ESP_LOGI(TAG, "Stopping Put Media");
            put_media_service_stop();
            res = put_media_service_start();
        }
    }

    return res;
}

static void put_media_task(void *pvParameters)
{
    assert(put_media_priv_data.mkv_buf_queue);

    size_t data_sent = 0;
    int64_t prev_time = 0;

    while (true) {
        uint32_t notification_value = 0;
        while (xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY) == pdTRUE
                && !(notification_value & START_TASK_SIGNAL)) { }

        esp_err_t res = put_media_service_start();

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
            if (notification_value & STOP_TASK_SIGNAL) {
                break;
            }

            if (xQueueReceive(put_media_priv_data.mkv_buf_queue, &mkv_buf_info, pdMS_TO_TICKS(100)) == pdTRUE) {
                put_media_err_t res;
                if ((res = put_media_update(mkv_buf_info)) != PUT_MEDIA_ERRNO_NONE) {
                    ESP_LOGE(TAG, "put media update failed: %d", res);
                    put_media_event_data_t ev = { .event = PUT_MEDIA_ON_ERROR, .data = { .error_code = res } };
                    put_media_notify_event(&ev);
                }
                data_sent += mkv_buf_info.uDataLen;
                if ((res = put_media_maintain()) != PUT_MEDIA_ERRNO_NONE) {
                    ESP_LOGE(TAG, "put media update failed: %d", res);
                    put_media_event_data_t ev = { .event = PUT_MEDIA_ON_ERROR, .data = { .error_code = res } };
                    put_media_notify_event(&ev);
                }
                free(mkv_buf_info.buffer);

                int64_t ctime = esp_timer_get_time();
                if (ctime - prev_time > 1000 * 1000 * 1) {
                    ESP_LOGI(TAG, "data sent: %d", data_sent);
                    prev_time = ctime;
                }
            }
        }

        put_media_service_stop();

        put_media_priv_data.state = TASK_STOPPED;
        data_sent = 0;
        {
            put_media_event_data_t ev = { .event = PUT_MEDIA_ON_STOP, .data = { 0 } };
            put_media_notify_event(&ev);
        }

        ESP_LOGI(TAG, "Put media stopped");
    }
}

put_media_err_t put_media_deinit()
{
    esp_err_t err = put_media_stop();

    if (err != PUT_MEDIA_ERRNO_NONE) {
        ESP_LOGE(TAG, "Could not stop put media");
        return err;
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
    if (put_media_priv_data.pAwsAccessKeyId != NULL) {
        free(put_media_priv_data.pAwsAccessKeyId);
        put_media_priv_data.pAwsAccessKeyId = NULL;
    }
    if (put_media_priv_data.pAwsSecretAccessKey != NULL) {
        free(put_media_priv_data.pAwsSecretAccessKey);
        put_media_priv_data.pAwsSecretAccessKey = NULL;
    }
    if (put_media_priv_data.pAwsSessionToken != NULL) {
        free(put_media_priv_data.pAwsSessionToken);
        put_media_priv_data.pAwsSessionToken = NULL;
    }
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

    if (put_media_priv_data.put_media_task_handle) {
        vTaskDelete(put_media_priv_data.put_media_task_handle);
        put_media_priv_data.put_media_task_handle = NULL;
    }

    if (put_media_priv_data.put_media_task_buffer) {
        free(put_media_priv_data.put_media_task_buffer);
        put_media_priv_data.put_media_task_buffer = NULL;
    }

    if (put_media_priv_data.put_media_task_stack) {
        free(put_media_priv_data.put_media_task_stack);
        put_media_priv_data.put_media_task_stack = NULL;
    }

    if (put_media_priv_data.mkv_buf_queue) {
        vQueueDelete(put_media_priv_data.mkv_buf_queue);
        put_media_priv_data.mkv_buf_queue = NULL;
    }

    put_media_priv_data.state = TASK_DEINITIATED;
    return PUT_MEDIA_ERRNO_NONE;
}
