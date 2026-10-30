/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_CLASS "ClockSkew"

/* strptime() is an XSI extension; glibc (the linux test target) declares it only on request */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <time.h>

#include "aws_clock_skew.h"

static inline int is_leap_year(int y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

/* UTC tm-to-epoch converter (adapted from ESP-RainMaker rmaker_timegm). mktime() would
 * read the tm as local time; the Date header is GMT. */
static INT64 utc_tm_to_epoch(struct tm *tm)
{
    int year  = tm->tm_year + 1900;
    int month = tm->tm_mon;

    /* Normalize month */
    if (month < 0) {
        year += (month - 11) / 12;
        month = 12 + (month % 12);
    } else if (month > 11) {
        year += month / 12;
        month %= 12;
    }

    static const int mdays_cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

    INT64 days = 0;
    if (year >= 1970) {
        for (int y = 1970; y < year; ++y) {
            days += 365 + is_leap_year(y);
        }
    } else {
        for (int y = year; y < 1970; ++y) {
            days -= 365 + is_leap_year(y);
        }
    }

    days += mdays_cum[month];
    if (month > 1 && is_leap_year(year)) {
        days += 1;
    }
    days += (tm->tm_mday - 1);

    return days * 86400LL + (INT64) tm->tm_hour * 3600 + (INT64) tm->tm_min * 60 + (INT64) tm->tm_sec;
}

STATUS awsClockSkewFromDate(PCHAR pDate, UINT64 now, PUINT64 pClockSkew, PUINT64 pServerTime)
{
    STATUS retStatus = STATUS_SUCCESS;
    struct tm tm = {0};
    INT64 serverEpoch;
    UINT64 serverTime, diff;

    CHK(pDate != NULL && pClockSkew != NULL, STATUS_NULL_ARG);
    *pClockSkew = 0;

    CHK(strptime(pDate, "%a, %d %b %Y %H:%M:%S GMT", &tm) != NULL, STATUS_INVALID_ARG);
    serverEpoch = utc_tm_to_epoch(&tm);
    CHK(serverEpoch >= 0, STATUS_INVALID_ARG);

    serverTime = (UINT64) serverEpoch * HUNDREDS_OF_NANOS_IN_A_SECOND;
    if (pServerTime != NULL) {
        *pServerTime = serverTime;
    }

    if (serverTime > now) {
        diff = serverTime - now;
        if (diff > AWS_CLOCK_SKEW_THRESHOLD) {
            *pClockSkew = diff;
        }
    } else {
        diff = now - serverTime;
        if (diff > AWS_CLOCK_SKEW_THRESHOLD) {
            *pClockSkew = diff | AWS_CLOCK_SKEW_DEVICE_AHEAD;
        }
    }

CleanUp:
    return retStatus;
}

VOID awsApplyClockSkew(PRequestInfo pRequestInfo, UINT64 clockSkew)
{
    if (pRequestInfo == NULL || clockSkew == 0) {
        return;
    }

    if (clockSkew & AWS_CLOCK_SKEW_DEVICE_AHEAD) {
        pRequestInfo->currentTime -= clockSkew & ~AWS_CLOCK_SKEW_DEVICE_AHEAD;
    } else {
        pRequestInfo->currentTime += clockSkew;
    }
}
