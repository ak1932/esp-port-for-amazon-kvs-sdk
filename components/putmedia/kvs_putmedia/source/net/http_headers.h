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

#ifndef HTTP_HEADERS_H
#define HTTP_HEADERS_H

#include <stddef.h>

/**
 * @brief An ordered collection of HTTP request headers.
 *
 * The collection is only ever consumed two ways: iterated to feed
 * esp_http_client_set_header(), and looked up by name while building the SigV4
 * canonical request. Names are matched case-insensitively, per RFC 9110.
 */
typedef struct HttpHeaders HttpHeaders_t;

/**
 * @brief Create an empty header collection.
 *
 * @return The new collection, or NULL on out of memory.
 */
HttpHeaders_t *HttpHeaders_create(void);

/**
 * @brief Release a header collection and every name/value it owns.
 *
 * @param[in] pxHeaders The collection, may be NULL
 */
void HttpHeaders_free(HttpHeaders_t *pxHeaders);

/**
 * @brief Add a header, replacing any existing header of the same name.
 *
 * Both the name and the value are copied.
 *
 * @param[in] pxHeaders The collection
 * @param[in] pcName Header name
 * @param[in] pcValue Header value
 * @return 0 on success, non-zero put_media error otherwise
 */
int HttpHeaders_add(HttpHeaders_t *pxHeaders, const char *pcName, const char *pcValue);

/**
 * @brief Look up a header value by name.
 *
 * @param[in] pxHeaders The collection
 * @param[in] pcName Header name
 * @return The value, owned by the collection, or NULL if there is no such header.
 */
const char *HttpHeaders_find(const HttpHeaders_t *pxHeaders, const char *pcName);

/**
 * @brief Number of headers in the collection.
 *
 * @param[in] pxHeaders The collection, may be NULL
 * @return The count, or 0 if the collection is NULL.
 */
size_t HttpHeaders_count(const HttpHeaders_t *pxHeaders);

/**
 * @brief Fetch the header at an index, in insertion order.
 *
 * @param[in] pxHeaders The collection
 * @param[in] uIndex Index, less than HttpHeaders_count()
 * @param[out] ppcName Header name, owned by the collection
 * @param[out] ppcValue Header value, owned by the collection
 * @return 0 on success, non-zero put_media error otherwise
 */
int HttpHeaders_get(const HttpHeaders_t *pxHeaders, size_t uIndex, const char **ppcName, const char **ppcValue);

#endif /* HTTP_HEADERS_H */
