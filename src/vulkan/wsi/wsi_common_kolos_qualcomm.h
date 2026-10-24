/* Copyright © 2026 Kolos contributors
 * SPDX-License-Identifier: MIT
 * Observed Qualcomm native_handle compatibility path, not a generic Mesa ABI.
 */
#ifndef WSI_COMMON_KOLOS_QUALCOMM_H
#define WSI_COMMON_KOLOS_QUALCOMM_H

#include "wsi_common_kolos.h"
#include <stddef.h>

/* Caller fills id, width, height, format and stride from trusted buffer
 * metadata.  Exactly 148 native-handle bytes and two borrowed FDs are
 * accepted.  On success the descriptor can be passed to import_buffer.
 */
VkResult
wsi_kolos_qualcomm_linear_from_handle(const uint8_t *bytes, size_t length,
                                      const int *fds, size_t fd_count,
                                      struct wsi_kolos_buffer_desc *desc);

#endif
