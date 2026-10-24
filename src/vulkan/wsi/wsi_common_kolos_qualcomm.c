/* Copyright © 2026 Kolos contributors
 * SPDX-License-Identifier: MIT
 * The exact observed KolosLinux handle family; unknown layouts fail closed.
 */
#include "wsi_common_kolos_qualcomm.h"

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t
read_le32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

VkResult
wsi_kolos_qualcomm_linear_from_handle(const uint8_t *bytes, size_t length,
                                      const int *fds, size_t fd_count,
                                      struct wsi_kolos_buffer_desc *desc)
{
   if (!bytes || !fds || !desc || length != 148 || fd_count != 2 ||
       fds[0] < 0 || fds[1] < 0 || !desc->width || !desc->height ||
       desc->format != 1 || desc->stride < desc->width ||
       desc->stride > UINT32_MAX / 4)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   uint32_t w[37];
   for (unsigned i = 0; i < 37; i++)
      w[i] = read_le32(bytes + 4 * i);
   if (w[0] != 0x47423031 || w[1] != desc->width ||
       w[2] != desc->height || w[3] != desc->stride ||
       w[4] != 1 || w[5] != 1 || w[6] != 0xb00 ||
       w[10] != 2 || w[11] != 24 || w[13] != 0x676d736d ||
       w[14] != 0x00100288 || w[15] != desc->stride ||
       w[16] < desc->height || w[17] != desc->width ||
       w[18] != desc->height || w[19] != 1 || w[20] != 0 ||
       w[21] != 1 || w[24] != 0xb00 || w[25] != 0 ||
       w[27] != 0 || w[28] != 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   uint64_t allocation = (uint64_t)desc->stride * w[16] * 4;
   uint64_t visible_minimum = (uint64_t)desc->stride * desc->height * 4;
   if (allocation > UINT32_MAX || visible_minimum > UINT32_MAX ||
       w[26] != allocation)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct stat st;
   if (fstat(fds[0], &st) != 0 || st.st_size < 0 ||
       (uint64_t)st.st_size != allocation)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   desc->modifier = 0;
   desc->offset = 0;
   desc->row_pitch = (uint64_t)desc->stride * 4;
   desc->allocation_size = allocation;
   desc->pixel_fd = fds[0];
   return VK_SUCCESS;
}
