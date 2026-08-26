#pragma once

#include "kvs/mkvapp.h"
#include <sys/_intsup.h>
#include <stdint.h>
#include "kvs/put_media_errors.h"
#include "kvs/restapi.h"

/**
 * @file put_media.h
 * @brief AWS Kinesis Video Streams (KVS) PutMedia streaming interface.
 *
 * This component provides a lightweight interface to stream MKV-formatted
 * media data to an AWS Kinesis Video Stream using the PutMedia API.
 *
 * The module is responsible for:
 *  - Initializing the PutMedia client with AWS and stream configuration
 *  - Fetching AWS credentials via a user-provided callback
 *  - Sending EBML headers and MKV fragments
 *  - Managing the lifecycle of a PutMedia streaming session
 */

/**
 * @brief Callback to provide the EBML header for the MKV stream.
 *
 * This callback must populate the provided mkv_buf_info_t with the
 * EBML header data required at the start of a Kinesis Video Stream.
 *
 * @param mkv_buf_info Pointer to a buffer descriptor that should be
 *                     filled with EBML header data.
 *
 * @return
 *  - PUT_MEDIA_ERRNO_NONE on success
 *  - Appropriate PUT_MEDIA_ERROR_* value on failure
 */
typedef put_media_err_t (*get_mkv_ebml_hdr_func)(mkv_buf_info_t *mkv_buf_info);

/**
 * @brief Callback used to fetch AWS credentials for Kinesis Video Streams.
 *
 * This callback is invoked by the PutMedia component whenever AWS credentials
 * are required or refreshed.
 *
 * The implementation must return valid AWS credentials and their lengths.
 * Session token may be NULL if not using temporary credentials.
 *
 * @param customData        User-defined data passed to the callback.
 * @param pAccessKey        Output pointer to AWS Access Key ID.
 * @param pAccessKeyLen     Length of the Access Key ID.
 * @param pSecretKey        Output pointer to AWS Secret Access Key.
 * @param pSecretKeyLen     Length of the Secret Access Key.
 * @param pSessionToken     Output pointer to AWS Session Token (optional).
 * @param pSessionTokenLen  Length of the Session Token.
 * @param pExpiration       Credential expiration time in seconds from now.
 *
 * @return
 *  - 0 on success
 *  - Non-zero value on failure
 */
typedef put_media_err_t (*kvs_fetch_credentials_cb_t)(
    uint64_t customData,
    const char **pAccessKey,
    uint32_t *pAccessKeyLen,
    const char **pSecretKey,
    uint32_t *pSecretKeyLen,
    const char **pSessionToken,
    uint32_t *pSessionTokenLen,
    uint32_t *pExpiration
);

/**
 * @brief PutMedia lifecycle and notification event types.
 */
typedef enum {
    PUT_MEDIA_ON_START,
    PUT_MEDIA_ON_STOP,
    PUT_MEDIA_ON_ERROR,
    PUT_MEDIA_ON_ACK_RECV,
} put_media_event_type_t;

/**
 * @brief Event data for PUT_MEDIA_ON_ACK_RECV.
 * Mirrors FragmentAck_t payload from kvs/restapi (ePutMediaFragmentAckEventType).
 */
typedef struct {
    ePutMediaFragmentAckEventType eventType;
    uint64_t uFragmentTimecode;
    unsigned int uErrorId;
} put_media_ack_event_data_t;

/**
 * @brief Event data passed to the event handler; union discriminated by event type.
 */
typedef struct {
    put_media_event_type_t event;
    union {
        put_media_err_t error_code;       /**< For PUT_MEDIA_ON_ERROR */
        put_media_ack_event_data_t ack;   /**< For PUT_MEDIA_ON_ACK_RECV */
    } data;
} put_media_event_data_t;

/**
 * @brief Optional event handler invoked for PutMedia lifecycle and ACK events.
 *
 * @param event_data Event type and associated data.
 * @param user_ctx User context passed at init.
 */
typedef void (*put_media_event_handler_t)(const put_media_event_data_t *event_data, void *user_ctx);

// TODO: Add support for IoT credentials
// TODO: Add NetIO parameters

/**
 * @brief Configuration structure for initializing the PutMedia component.
 *
 * This structure defines AWS endpoint information, stream details,
 * credential callbacks, and MKV-related configuration.
 */
typedef struct {
    /** AWS Kinesis Video endpoint hostname */
    const char *pcHost;

    /** AWS region (e.g., "us-west-2") */
    const char *pcRegion;

    /** AWS service name (typically "kinesisvideo") */
    const char *pcService;

    /** Name of the Kinesis Video Stream */
    const char *pcStreamName;

    /** Callback to provide the MKV EBML header */
    get_mkv_ebml_hdr_func get_mkv_ebml_hdr_func;

    /** Callback to fetch AWS credentials */
    kvs_fetch_credentials_cb_t kvs_fetch_credentials_cb;

    /** Stream data retention period in hours */
    unsigned int uDataRetentionHours;

    /** Optional event handler for PUT_MEDIA_ON_START, PUT_MEDIA_ON_STOP, PUT_MEDIA_ON_ERROR, PUT_MEDIA_ON_ACK_RECV (may be NULL) */
    put_media_event_handler_t event_handler;

    /** User context passed to event_handler (optional) */
    void *event_handler_user_ctx;
} put_media_config_t;

/**
 * @brief Initialize the PutMedia streaming component.
 *
 * This function must be called before starting a PutMedia session.
 *
 * @param config PutMedia configuration structure.
 *
 * @return
 *  - PUT_MEDIA_ERRNO_NONE on success
 *  - PUT_MEDIA_ERROR_* on failure
 */
put_media_err_t put_media_init(put_media_config_t config);

/**
 * @brief Check whether the PutMedia stream is currently active.
 *
 * @return
 *  - true if streaming has started
 *  - false otherwise
 */
bool put_media_is_started();

/**
 * @brief Start streaming task to the Kinesis Video Stream using PutMedia.
 *
 * This function only signals the task to run and returns.
 * The task will then start asynchronously, whether the task
 * was started can be queried with put_media_is_started()
 *
 * @return
 *  - PUT_MEDIA_ERRNO_NONE on success
 *  - PUT_MEDIA_ERROR_* on failure
 */
put_media_err_t put_media_start();

/**
 * @brief Enqueue MKV data for transmission to Kinesis Video Streams.
 *
 * The provided buffer is expected to contain valid MKV data such as
 * clusters or blocks.
 *
 * @param mkv_buf_info MKV buffer descriptor containing encoded data.
 *
 * @return
 *  - PUT_MEDIA_ERRNO_NONE on success
 *  - PUT_MEDIA_ERROR_* on failure
 */
put_media_err_t put_media_enqueue(mkv_buf_info_t mkv_buf_info);

/**
 * @brief Stop the active PutMedia streaming session.
 *
 * This gracefully terminates the connection to AWS KVS.
 *
 * @return
 *  - PUT_MEDIA_ERRNO_NONE on success
 *  - PUT_MEDIA_ERROR_* on failure
 */
put_media_err_t put_media_stop();

/**
 * @brief Deinitialize the PutMedia component and free resources.
 *
 * This should be called when the component is no longer needed.
 *
 * @return
 *  - PUT_MEDIA_ERRNO_NONE on success
 *  - PUT_MEDIA_ERROR_* on failure
 */
put_media_err_t put_media_deinit();
