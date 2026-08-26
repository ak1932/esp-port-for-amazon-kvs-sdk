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

#ifndef JSON_HELPER_H
#define JSON_HELPER_H

#include <inttypes.h>

#include "cJSON.h"

/**
 * @brief Look up a dot-separated path and return a copy of its value as a string.
 *
 * cJSON has no dot-path getter, so the path is walked one segment at a time with
 * cJSON_GetObjectItemCaseSensitive(). A JSON string is returned unquoted; any other
 * kind of value is returned in its serialized form.
 *
 * @param[in] pxRoot The parsed JSON object
 * @param[in] pcPath Dot-separated path, e.g. "credentials.accessKeyId"
 * @return The value on success (caller must kvsFree), or NULL on error.
 */
char *json_dotget_string(const cJSON *pxRoot, const char *pcPath);

/**
 * @brief Look up a dot-separated path and return its value as a uint64_t.
 *
 * Accepts both a JSON number and a JSON string, since KVS is inconsistent about
 * which one it sends for a timecode.
 *
 * @param[in] pxRoot The parsed JSON object
 * @param[in] pcPath Dot-separated path, e.g. "FragmentTimecode"
 * @param[in] xBase Numeric base used when the value is a JSON string
 * @return The value, or 0 if the path is absent or the value is not convertible.
 */
uint64_t json_dotget_uint64(const cJSON *pxRoot, const char *pcPath, int xBase);

#endif
