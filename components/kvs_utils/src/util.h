/*******************************************
Util internal include file
*******************************************/
#ifndef __KINESIS_VIDEO_UTIL_INCLUDE_I__
#define __KINESIS_VIDEO_UTIL_INCLUDE_I__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JSMN_HEADER
#define JSMN_HEADER
#endif
#include "jsmn.h"

#define EARLY_EXPIRATION_FACTOR              1.0
#define IOT_EXPIRATION_PARSE_CONVERSION_BASE 10

#define SSL_CERTIFICATE_TYPE_UNKNOWN_STR ((PCHAR) "Unknown")
#define SSL_CERTIFICATE_TYPE_DER_STR     ((PCHAR) "DER")
#define SSL_CERTIFICATE_TYPE_ENG_STR     ((PCHAR) "ENG")
#define SSL_CERTIFICATE_TYPE_PEM_STR     ((PCHAR) "PEM")

UINT64 commonDefaultGetCurrentTimeFunc(UINT64);
/**
 * Compares JSON strings taking into account the type
 *
 * @param - PCHAR - IN - JSON string being parsed
 * @param - jsmntok_t* - IN - Jsmn token to match
 * @param - jsmntype_t - IN - Jsmn token type to match
 * @param - PCHAR - IN - Token name to match
 *
 * @return - STATUS code of the execution
 */
BOOL compareJsonString(PCHAR, jsmntok_t*, jsmntype_t, PCHAR);
/**
 * @brief Builds the KVS control-plane URL for a region, e.g. https://kinesisvideo.us-east-1.amazonaws.com,
 * with the FIPS service name and the CN/ISO/ISO-B domains where they apply
 *
 * @param[in] pRegion AWS region
 * @param[out] pUrl Buffer for the URL
 * @param[in] urlLen Buffer size, including the terminator
 *
 * @return - STATUS code of the execution
 */
STATUS getKvsControlPlaneUrl(PCHAR pRegion, PCHAR pUrl, UINT32 urlLen);
/**
 * @brief Converts the timestamp string to time
 *
 * @param[in] expirationTimestampStr String to covert/compare
 * @param[in] nowTime Current time to compare
 * @param[in, out] pExpiration Converted time
 *
 * @return STATUS code of the execution
 */
UINT32 convertTimestampToEpoch(PCHAR expirationTimestampStr, UINT64 nowTime, PUINT64 pExpiration);

#ifdef __cplusplus
}
#endif
#endif /* __KINESIS_VIDEO_UTIL_INCLUDE_I__ */
