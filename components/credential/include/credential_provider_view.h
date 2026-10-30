/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __CREDENTIAL_PROVIDER_VIEW_H__
#define __CREDENTIAL_PROVIDER_VIEW_H__

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "common_defs.h"

/*
 * Sharing a provider: the static, IoT, callback and file providers can serve several users.
 * Credentials they hand out stay valid until their refresh after next, which is enough for a
 * user that takes them just before signing. A user that keeps the pointer longer, as signaling
 * keeps the one from its GET_TOKEN state, takes a view instead.
 */

/**
 * @brief Creates a view of a shared provider: a provider for one user that hands out its own
 * copy of the shared provider's credentials
 *
 * Each get asks the shared provider and copies its credentials when they changed. A copy stays
 * valid until the view's own get after next, whatever the shared provider's other users do.
 *
 * @param[in] pShared The shared provider; it must outlive the view, or the view's detach
 * @param[out] ppView The view
 *
 * @return STATUS code of the execution. STATUS_SUCCESS on success
 */
STATUS createCredentialProviderView(PAwsCredentialProvider pShared, PAwsCredentialProvider* ppView);

/**
 * @brief Stops a view from using its shared provider: once this returns, the view's gets fail
 * and the shared provider can be freed. Its copies stay valid until the view is freed.
 */
VOID detachCredentialProviderView(PAwsCredentialProvider pView);

/**
 * @brief Frees a view and its copies, leaving the shared provider alone
 */
STATUS freeCredentialProviderView(PAwsCredentialProvider* ppView);

#ifdef __cplusplus
}
#endif

#endif /* __CREDENTIAL_PROVIDER_VIEW_H__ */
