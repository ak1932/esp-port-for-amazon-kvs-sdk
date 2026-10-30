/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __AWS_CLOCK_SKEW_H__
#define __AWS_CLOCK_SKEW_H__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "common_defs.h"

/* Differences up to this are left alone (100 ns units) */
#define AWS_CLOCK_SKEW_THRESHOLD (3 * 60 * HUNDREDS_OF_NANOS_IN_A_SECOND)

/* Set in a skew value when the device clock is ahead of the server's */
#define AWS_CLOCK_SKEW_DEVICE_AHEAD ((UINT64) 1ULL << 63)

/**
 * @brief Measures the device clock against an HTTP Date header ("Fri, 27 Jun 2025 12:27:54 GMT")
 *
 * @param[in] pDate Date header value
 * @param[in] now Device time, 100 ns units since the epoch (GETTIME())
 * @param[out] pClockSkew 0 within AWS_CLOCK_SKEW_THRESHOLD, otherwise the difference in 100 ns units,
 *                        with AWS_CLOCK_SKEW_DEVICE_AHEAD set when the device is ahead
 * @param[out] pServerTime Optional: the server time, 100 ns units since the epoch
 *
 * @return STATUS_SUCCESS, or STATUS_INVALID_ARG when the date does not parse
 */
STATUS awsClockSkewFromDate(PCHAR pDate, UINT64 now, PUINT64 pClockSkew, PUINT64 pServerTime);

/**
 * @brief Moves pRequestInfo->currentTime by a skew from awsClockSkewFromDate(), so a request signed
 * with it carries the server's notion of now
 */
VOID awsApplyClockSkew(PRequestInfo pRequestInfo, UINT64 clockSkew);

#ifdef __cplusplus
}
#endif

#endif /* __AWS_CLOCK_SKEW_H__ */
