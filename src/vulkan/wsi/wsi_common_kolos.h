/* Copyright © 2026 Kolos contributors
 * SPDX-License-Identifier: MIT
 *
 * Transport-neutral Kolos WSI import/lifecycle core.  A future private
 * Wayland backend owns wl_surface routing and delivers pool/buffer events.
 */
#ifndef WSI_COMMON_KOLOS_H
#define WSI_COMMON_KOLOS_H

#include "wsi_common_private.h"

struct wsi_kolos_session;
struct wsi_kolos_pool;

struct wsi_kolos_identity {
   uint64_t session_id;
   uint64_t generation;
   uint32_t slot;
};

struct wsi_kolos_pool_info {
   uint64_t session_id, generation;
   uint32_t width, height, format, slot_count;
};

/* All lengths are bytes.  The current proven import supports only RGBA8,
 * DRM modifier LINEAR (0), offset 0 and one plane.  The pixel FD is borrowed:
 * import duplicates it for Vulkan, then leaves the original with transport.
 */
struct wsi_kolos_buffer_desc {
   struct wsi_kolos_identity id;
   uint32_t width, height, format, stride;
   uint64_t modifier, offset, row_pitch, allocation_size;
   int pixel_fd;
};

struct wsi_kolos_image {
   struct wsi_image base;
   struct wsi_kolos_identity id;
   uint32_t width, height, stride;
   uint64_t modifier, offset, row_pitch, allocation_size;
};

/* These outbound operations are implemented by the future private Wayland
 * transport.  No Android graphics.sock connection belongs in production WSI.
 * Callbacks must not synchronously re-enter this session's event API.
 */
struct wsi_kolos_transport_ops {
   VkResult (*request_buffer)(void *data, struct wsi_kolos_identity id);
   VkResult (*pool_ready)(void *data, uint64_t session_id,
                          uint64_t generation);
   VkResult (*present)(void *data, struct wsi_kolos_identity id,
                       uint64_t serial);
};

VkResult
wsi_kolos_session_create(const struct wsi_device *wsi, VkDevice device,
                         PFN_vkGetMemoryFdPropertiesKHR get_fd_properties,
                         VkImageUsageFlags usage,
                         const VkAllocationCallbacks *allocator,
                         uint64_t session_id,
                         const struct wsi_kolos_transport_ops *transport,
                         void *transport_data,
                         struct wsi_kolos_session **out);

/* NEW_POOL/POOL_REPLACED: begin an unpublished generation, request every
 * buffer, then import every slot.  Only a complete pool may become current.
 */
VkResult
wsi_kolos_pool_create(struct wsi_kolos_session *session, uint64_t generation,
                      uint32_t width, uint32_t height, uint32_t format,
                      uint32_t slot_count, struct wsi_kolos_pool **out);
VkResult wsi_kolos_pool_request_buffers(struct wsi_kolos_pool *pool);
VkResult
wsi_kolos_import_buffer(struct wsi_kolos_pool *pool,
                        const struct wsi_kolos_buffer_desc *buffer);
VkResult wsi_kolos_pool_notify_ready(struct wsi_kolos_pool *pool);
VkResult wsi_kolos_pool_activate(struct wsi_kolos_pool *pool);
/* Discard an unpublished pool after partial import or transport failure. */
VkResult wsi_kolos_pool_destroy(struct wsi_kolos_pool *pool);

/* A pinned generation keeps its VkImages alive across replacement.  The
 * caller must unpin it before destroying the session.  Fields are immutable.
 */
VkResult wsi_kolos_pin_current_pool(struct wsi_kolos_session *session,
                                   struct wsi_kolos_pool **out);
struct wsi_kolos_pool_info
wsi_kolos_pool_get_info(const struct wsi_kolos_pool *pool);
const struct wsi_kolos_image *
wsi_kolos_pool_get_image(const struct wsi_kolos_pool *pool, uint32_t slot);
void wsi_kolos_unpin_pool(struct wsi_kolos_pool *pool);

/* Inbound FREE/ERROR events from the transport.  FREE is the sole release
 * confirmation permitting reuse; PRESENT/ACK alone never makes a slot free.
 */
VkResult
wsi_kolos_pool_set_available(struct wsi_kolos_session *session,
                             struct wsi_kolos_identity id);
void wsi_kolos_session_error(struct wsi_kolos_session *session, VkResult error);

/* Blocking acquire uses a condition variable; zero timeout returns NOT_READY.
 * The image lease lasts until present or release_image.  Transport owns the
 * queue/ownership synchronization before calling present; no rendering layout
 * is imposed by this core.
 */
VkResult
wsi_kolos_pool_acquire(struct wsi_kolos_session *session, uint64_t timeout_ns,
                       struct wsi_kolos_image **out);
VkResult
wsi_kolos_pool_presented(struct wsi_kolos_session *session,
                         struct wsi_kolos_image *image, uint64_t serial);
void wsi_kolos_release_image(struct wsi_kolos_session *session,
                             struct wsi_kolos_image *image);

/* A replacement automatically retires the previous current pool.  The
 * explicit form marks the surface out of date until a new pool activates. */
void wsi_kolos_pool_retire(struct wsi_kolos_session *session);
void wsi_kolos_session_destroy(struct wsi_kolos_session *session);

#endif
