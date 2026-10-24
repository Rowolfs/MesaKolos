/* Copyright © 2026 Kolos contributors
 * SPDX-License-Identifier: MIT
 *
 * Private Kolos graphics-v2 transport/import core.  It is deliberately not
 * registered as a VkSurfaceKHR platform yet.
 */
#ifndef WSI_COMMON_KOLOS_H
#define WSI_COMMON_KOLOS_H

#include "wsi_common_private.h"

struct wsi_kolos_connection;
struct wsi_kolos_pool;

struct wsi_kolos_pool_info {
   uint64_t session_id, generation;
   uint32_t width, height, slot_count;
};

struct wsi_kolos_image {
   struct wsi_image base;
   uint64_t session_id;
   uint64_t generation;
   uint32_t slot;
   uint32_t width, height, stride;
   uint64_t allocation_size;
};

/* get_fd_properties is supplied by the ICD.  The caller must enable the
 * external-memory-fd, dma-buf, DRM-modifier and foreign-queue extensions.
 * usage is the future swapchain's requested image usage, not a test layout.
 */
VkResult
wsi_kolos_connect(const char *socket_path, const struct wsi_device *wsi,
                  VkDevice device,
                  PFN_vkGetMemoryFdPropertiesKHR get_fd_properties,
                  VkImageUsageFlags usage,
                  const VkAllocationCallbacks *allocator,
                  struct wsi_kolos_connection **out);

/* Pin a complete imported generation while a future swapchain holds its
 * VkImage handles.  The pinned pool survives replacement until unpinned.
 */
VkResult wsi_kolos_pin_current_pool(struct wsi_kolos_connection *connection,
                                   struct wsi_kolos_pool **out);
struct wsi_kolos_pool_info
wsi_kolos_pool_get_info(const struct wsi_kolos_pool *pool);
const struct wsi_kolos_image *
wsi_kolos_pool_get_image(const struct wsi_kolos_pool *pool, uint32_t slot);
void wsi_kolos_unpin_pool(struct wsi_kolos_connection *connection,
                          struct wsi_kolos_pool *pool);

/* The returned image is leased until present or release_image.  timeout_ns
 * follows vkAcquireNextImageKHR's timeout convention.  The image identity is
 * always (session_id, generation, slot), never only a swapchain index.
 */
VkResult
wsi_kolos_wait_available(struct wsi_kolos_connection *connection,
                         uint64_t timeout_ns,
                         struct wsi_kolos_image **out);

/* The caller must finish GPU use and perform the required foreign ownership
 * transition before PRESENT.  This core does not impose an application layout.
 * Serial starts at zero and advances only after ACK.  Returns OUT_OF_DATE if
 * Android replaced the generation during this frame.
 */
VkResult
wsi_kolos_present(struct wsi_kolos_connection *connection,
                  struct wsi_kolos_image *image, uint64_t serial);

void
wsi_kolos_release_image(struct wsi_kolos_connection *connection,
                        struct wsi_kolos_image *image);

VkResult wsi_kolos_stop(struct wsi_kolos_connection *connection);
/* Destruction requires callers to have returned image leases and pool pins,
 * and to have stopped entering the API.  It wakes and joins the dispatcher.
 */
void wsi_kolos_destroy(struct wsi_kolos_connection *connection);

#endif
