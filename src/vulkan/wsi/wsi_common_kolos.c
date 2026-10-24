/* Copyright © 2026 Kolos contributors
 * SPDX-License-Identifier: MIT
 *
 * No Unix socket, Android wire opcode or wl_surface belongs in this file.
 * The private Wayland backend supplies buffer metadata and events.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "wsi_common_kolos.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KOLOS_MAX_SLOTS 16u

enum kolos_slot_state {
   KOLOS_WAIT_FREE,
   KOLOS_AVAILABLE,
   KOLOS_ACQUIRED,
   KOLOS_PRESENTED,
};

struct wsi_kolos_pool {
   struct wsi_kolos_session *session;
   struct wsi_kolos_pool *next;
   struct wsi_kolos_pool_info info;
   struct wsi_kolos_image *images;
   enum kolos_slot_state *state;
   uint32_t imported, leases, pins;
   bool ready_sent;
};

struct wsi_kolos_session {
   const struct wsi_device *wsi;
   VkDevice device;
   PFN_vkGetMemoryFdPropertiesKHR get_fd_properties;
   VkImageUsageFlags usage;
   VkAllocationCallbacks allocator;
   bool has_allocator;
   struct wsi_kolos_transport_ops transport;
   void *transport_data;
   uint64_t id, next_serial, latest_generation;
   struct wsi_kolos_pool *current, *staging, *retired;
   VkResult error;
   bool destroying, retired_without_current;
   pthread_mutex_t lock;
   pthread_cond_t changed;
};

static const VkAllocationCallbacks *
allocator(const struct wsi_kolos_session *session)
{
   return session->has_allocator ? &session->allocator : NULL;
}

static void
destroy_image(struct wsi_kolos_session *session, struct wsi_kolos_image *image)
{
   if (image->base.image != VK_NULL_HANDLE)
      session->wsi->DestroyImage(session->device, image->base.image,
                                 allocator(session));
   if (image->base.memory != VK_NULL_HANDLE)
      session->wsi->FreeMemory(session->device, image->base.memory,
                               allocator(session));
   image->base.image = VK_NULL_HANDLE;
   image->base.memory = VK_NULL_HANDLE;
}

static void
destroy_pool(struct wsi_kolos_pool *pool)
{
   if (!pool)
      return;
   for (uint32_t i = 0; i < pool->info.slot_count; i++)
      destroy_image(pool->session, &pool->images[i]);
   free(pool->images);
   free(pool->state);
   free(pool);
}

static struct wsi_kolos_pool *
find_pool(struct wsi_kolos_session *session, uint64_t generation)
{
   if (session->current && session->current->info.generation == generation)
      return session->current;
   if (session->staging && session->staging->info.generation == generation)
      return session->staging;
   for (struct wsi_kolos_pool *pool = session->retired; pool; pool = pool->next)
      if (pool->info.generation == generation)
         return pool;
   return NULL;
}

static void
collect_retired(struct wsi_kolos_session *session)
{
   struct wsi_kolos_pool **link = &session->retired;
   while (*link) {
      struct wsi_kolos_pool *pool = *link;
      bool busy = pool->pins || pool->leases;
      for (uint32_t i = 0; i < pool->info.slot_count; i++)
         busy |= pool->state[i] != KOLOS_AVAILABLE;
      if (busy) {
         link = &pool->next;
      } else {
         *link = pool->next;
         destroy_pool(pool);
      }
   }
}

static VkResult
set_error_locked(struct wsi_kolos_session *session, VkResult error)
{
   if (error == VK_SUCCESS)
      error = VK_ERROR_SURFACE_LOST_KHR;
   if (session->error == VK_SUCCESS)
      session->error = error;
   pthread_cond_broadcast(&session->changed);
   return session->error;
}

static VkFormatFeatureFlags
required_features(VkImageUsageFlags usage)
{
   VkFormatFeatureFlags features = 0;
   if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
   if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
      features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
   if (usage & VK_IMAGE_USAGE_STORAGE_BIT)
      features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
   if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
      features |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
   if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
      features |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
   return features;
}

static VkResult
validate_buffer(const struct wsi_kolos_pool *pool,
                const struct wsi_kolos_buffer_desc *desc)
{
   const struct wsi_kolos_pool_info *info = &pool->info;
   if (desc->id.session_id != info->session_id ||
       desc->id.generation != info->generation ||
       desc->id.slot >= info->slot_count ||
       desc->width != info->width || desc->height != info->height ||
       desc->format != info->format || desc->format != 1 ||
       desc->stride < desc->width || desc->stride > UINT32_MAX / 4 ||
       desc->modifier != 0 || desc->offset != 0 ||
       desc->row_pitch != (uint64_t)desc->stride * 4 ||
       desc->allocation_size > UINT32_MAX ||
       desc->allocation_size < desc->row_pitch * desc->height ||
       desc->pixel_fd < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct stat st;
   if (fstat(desc->pixel_fd, &st) != 0 || st.st_size < 0 ||
       (uint64_t)st.st_size != desc->allocation_size)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   return VK_SUCCESS;
}

/* The same explicit-linear dma-buf import path proven by KolosLinux.  The
 * layout descriptor is supplied by transport and checked against the FD and
 * Vulkan modifier/format capabilities before the import. */
static VkResult
import_image(struct wsi_kolos_session *session, struct wsi_kolos_image *image,
             const struct wsi_kolos_buffer_desc *desc)
{
   const struct wsi_device *wsi = session->wsi;
   const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
   const VkExternalMemoryHandleTypeFlagBits handle_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   if (!wsi->GetPhysicalDeviceFormatProperties2 ||
       !wsi->GetPhysicalDeviceImageFormatProperties2 ||
       !wsi->CreateImage || !wsi->GetImageMemoryRequirements ||
       !wsi->AllocateMemory || !wsi->BindImageMemory ||
       !wsi->DestroyImage || !wsi->FreeMemory ||
       !session->get_fd_properties)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   VkDrmFormatModifierPropertiesListEXT modifiers = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
   };
   VkFormatProperties2 format_props = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &modifiers,
   };
   wsi->GetPhysicalDeviceFormatProperties2(wsi->pdevice, format, &format_props);
   uint32_t count = modifiers.drmFormatModifierCount;
   if (!count || count > 1024)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkDrmFormatModifierPropertiesEXT *list = calloc(count, sizeof(*list));
   if (!list)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   modifiers.pDrmFormatModifierProperties = list;
   wsi->GetPhysicalDeviceFormatProperties2(wsi->pdevice, format, &format_props);
   bool supported = false;
   VkFormatFeatureFlags needed = required_features(session->usage);
   if (modifiers.drmFormatModifierCount <= count) {
      for (uint32_t i = 0; i < modifiers.drmFormatModifierCount; i++) {
         if (list[i].drmFormatModifier == desc->modifier &&
             list[i].drmFormatModifierPlaneCount == 1 &&
             (list[i].drmFormatModifierTilingFeatures & needed) == needed) {
            supported = true;
            break;
         }
      }
   }
   free(list);
   if (!supported)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
      .drmFormatModifier = desc->modifier,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkPhysicalDeviceExternalImageFormatInfo external_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .pNext = &mod_query, .handleType = handle_type,
   };
   VkPhysicalDeviceImageFormatInfo2 query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &external_query, .format = format,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = session->usage,
   };
   VkExternalImageFormatProperties external_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
   };
   VkImageFormatProperties2 properties = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      .pNext = &external_props,
   };
   VkResult result = wsi->GetPhysicalDeviceImageFormatProperties2(
      wsi->pdevice, &query, &properties);
   if (result != VK_SUCCESS)
      return result;
   if (!(external_props.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
       properties.imageFormatProperties.maxExtent.width < desc->width ||
       properties.imageFormatProperties.maxExtent.height < desc->height)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   VkSubresourceLayout layout = {
      .offset = desc->offset, .rowPitch = desc->row_pitch,
   };
   VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_mod = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = desc->modifier,
      .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &layout,
   };
   VkExternalMemoryImageCreateInfo external_image = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &explicit_mod, .handleTypes = handle_type,
   };
   VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external_image, .imageType = VK_IMAGE_TYPE_2D,
      .format = format, .extent = { desc->width, desc->height, 1 },
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = session->usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = wsi->CreateImage(session->device, &image_info,
                              allocator(session), &image->base.image);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements req;
   wsi->GetImageMemoryRequirements(session->device, image->base.image, &req);
   if (req.size > desc->allocation_size)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   result = session->get_fd_properties(session->device, handle_type,
                                       desc->pixel_fd, &fd_props);
   if (result != VK_SUCCESS)
      return result;
   uint32_t bits = req.memoryTypeBits & fd_props.memoryTypeBits;
   if (!bits)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   uint32_t memory_type = 0;
   while (!(bits & (1u << memory_type)))
      memory_type++;
   int import_fd = fcntl(desc->pixel_fd, F_DUPFD_CLOEXEC, 0);
   if (import_fd < 0)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->base.image,
   };
   VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated, .handleType = handle_type, .fd = import_fd,
   };
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import, .allocationSize = req.size,
      .memoryTypeIndex = memory_type,
   };
   result = wsi->AllocateMemory(session->device, &alloc,
                                 allocator(session), &image->base.memory);
   if (result != VK_SUCCESS) {
      close(import_fd); /* Vulkan owns the duplicate only on success. */
      return result;
   }
   result = wsi->BindImageMemory(session->device, image->base.image,
                                 image->base.memory, 0);
   if (result != VK_SUCCESS)
      return result;
   image->base.dma_buf_fd = -1; /* Transport retains the original FD. */
   image->base.drm_modifier = desc->modifier;
   image->base.num_planes = 1;
   image->base.sizes[0] = (uint32_t)desc->allocation_size;
   image->base.offsets[0] = (uint32_t)desc->offset;
   image->base.row_pitches[0] = (uint32_t)desc->row_pitch;
   return VK_SUCCESS;
}

VkResult
wsi_kolos_session_create(const struct wsi_device *wsi, VkDevice device,
                         PFN_vkGetMemoryFdPropertiesKHR get_fd_properties,
                         VkImageUsageFlags usage,
                         const VkAllocationCallbacks *alloc,
                         uint64_t session_id,
                         const struct wsi_kolos_transport_ops *transport,
                         void *transport_data,
                         struct wsi_kolos_session **out)
{
   if (!wsi || !get_fd_properties || !usage || !session_id ||
       !transport || !transport->request_buffer || !transport->pool_ready ||
       !transport->present || !out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   struct wsi_kolos_session *session = calloc(1, sizeof(*session));
   if (!session)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   session->wsi = wsi;
   session->device = device;
   session->get_fd_properties = get_fd_properties;
   session->usage = usage;
   session->id = session_id;
   session->transport = *transport;
   session->transport_data = transport_data;
   if (alloc) {
      session->allocator = *alloc;
      session->has_allocator = true;
   }
   if (pthread_mutex_init(&session->lock, NULL) != 0) {
      free(session);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   if (pthread_cond_init(&session->changed, &attr) != 0) {
      pthread_condattr_destroy(&attr);
      pthread_mutex_destroy(&session->lock);
      free(session);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   pthread_condattr_destroy(&attr);
   *out = session;
   return VK_SUCCESS;
}

VkResult
wsi_kolos_pool_create(struct wsi_kolos_session *session, uint64_t generation,
                      uint32_t width, uint32_t height, uint32_t format,
                      uint32_t slot_count, struct wsi_kolos_pool **out)
{
   if (!session || !out || !generation || !width || !height ||
       format != 1 || !slot_count || slot_count > KOLOS_MAX_SLOTS)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   *out = NULL;
   struct wsi_kolos_pool *pool = calloc(1, sizeof(*pool));
   if (!pool)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   pool->images = calloc(slot_count, sizeof(*pool->images));
   pool->state = calloc(slot_count, sizeof(*pool->state));
   if (!pool->images || !pool->state) {
      destroy_pool(pool);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   pool->session = session;
   pool->info = (struct wsi_kolos_pool_info){
      .session_id = session->id, .generation = generation,
      .width = width, .height = height, .format = format,
      .slot_count = slot_count,
   };
   pthread_mutex_lock(&session->lock);
   VkResult result = VK_SUCCESS;
   if (session->destroying || session->error != VK_SUCCESS ||
       session->staging || find_pool(session, generation) ||
       generation <= session->latest_generation)
      result = VK_ERROR_OUT_OF_DATE_KHR;
   else
      session->staging = pool;
   pthread_mutex_unlock(&session->lock);
   if (result != VK_SUCCESS) {
      destroy_pool(pool);
      return result;
   }
   *out = pool;
   return VK_SUCCESS;
}

VkResult
wsi_kolos_pool_request_buffers(struct wsi_kolos_pool *pool)
{
   if (!pool)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct wsi_kolos_session *session = pool->session;
   pthread_mutex_lock(&session->lock);
   bool valid = !session->destroying && session->error == VK_SUCCESS &&
                session->staging == pool;
   pthread_mutex_unlock(&session->lock);
   if (!valid)
      return VK_ERROR_OUT_OF_DATE_KHR;
   for (uint32_t i = 0; i < pool->info.slot_count; i++) {
      struct wsi_kolos_identity id = {
         pool->info.session_id, pool->info.generation, i,
      };
      VkResult result = session->transport.request_buffer(
         session->transport_data, id);
      if (result != VK_SUCCESS) {
         wsi_kolos_session_error(session, result);
         return result;
      }
   }
   return VK_SUCCESS;
}

VkResult
wsi_kolos_import_buffer(struct wsi_kolos_pool *pool,
                        const struct wsi_kolos_buffer_desc *desc)
{
   if (!pool || !desc)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct wsi_kolos_session *session = pool->session;
   pthread_mutex_lock(&session->lock);
   VkResult result = session->error;
   if (result == VK_SUCCESS && (session->destroying ||
       session->staging != pool || pool->ready_sent))
      result = VK_ERROR_OUT_OF_DATE_KHR;
   if (result == VK_SUCCESS)
      result = validate_buffer(pool, desc);
   uint32_t slot = desc->id.slot;
   if (result == VK_SUCCESS && pool->images[slot].base.image != VK_NULL_HANDLE)
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
   if (result == VK_SUCCESS) {
      struct wsi_kolos_image *image = &pool->images[slot];
      result = import_image(session, image, desc);
      if (result == VK_SUCCESS) {
         image->id = desc->id;
         image->width = desc->width;
         image->height = desc->height;
         image->stride = desc->stride;
         image->modifier = desc->modifier;
         image->offset = desc->offset;
         image->row_pitch = desc->row_pitch;
         image->allocation_size = desc->allocation_size;
         pool->imported++;
      } else {
         destroy_image(session, image);
      }
   }
   pthread_mutex_unlock(&session->lock);
   return result;
}

VkResult
wsi_kolos_pool_notify_ready(struct wsi_kolos_pool *pool)
{
   if (!pool)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct wsi_kolos_session *session = pool->session;
   pthread_mutex_lock(&session->lock);
   bool valid = !session->destroying && session->error == VK_SUCCESS &&
                session->staging == pool &&
                pool->imported == pool->info.slot_count &&
                !pool->ready_sent;
   if (valid)
      pool->ready_sent = true;
   pthread_mutex_unlock(&session->lock);
   if (!valid)
      return VK_ERROR_OUT_OF_DATE_KHR;
   VkResult result = session->transport.pool_ready(
      session->transport_data, session->id, pool->info.generation);
   if (result != VK_SUCCESS)
      wsi_kolos_session_error(session, result);
   return result;
}

VkResult
wsi_kolos_pool_activate(struct wsi_kolos_pool *pool)
{
   if (!pool)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct wsi_kolos_session *session = pool->session;
   pthread_mutex_lock(&session->lock);
   VkResult result = session->error;
   if (result == VK_SUCCESS && (session->destroying ||
       session->staging != pool || !pool->ready_sent ||
       pool->imported != pool->info.slot_count))
      result = VK_ERROR_OUT_OF_DATE_KHR;
   if (result == VK_SUCCESS) {
      if (session->current) {
         session->current->next = session->retired;
         session->retired = session->current;
      }
      session->current = pool;
      session->staging = NULL;
      session->latest_generation = pool->info.generation;
      session->retired_without_current = false;
      collect_retired(session);
      pthread_cond_broadcast(&session->changed);
   }
   pthread_mutex_unlock(&session->lock);
   return result;
}

VkResult
wsi_kolos_pool_destroy(struct wsi_kolos_pool *pool)
{
   if (!pool)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct wsi_kolos_session *session = pool->session;
   pthread_mutex_lock(&session->lock);
   if (session->staging != pool) {
      pthread_mutex_unlock(&session->lock);
      return VK_ERROR_OUT_OF_DATE_KHR;
   }
   session->staging = NULL;
   pthread_mutex_unlock(&session->lock);
   destroy_pool(pool);
   return VK_SUCCESS;
}

VkResult
wsi_kolos_pin_current_pool(struct wsi_kolos_session *session,
                           struct wsi_kolos_pool **out)
{
   if (!session || !out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   pthread_mutex_lock(&session->lock);
   VkResult result = session->error;
   if (result == VK_SUCCESS && session->current && !session->destroying) {
      session->current->pins++;
      *out = session->current;
   } else if (result == VK_SUCCESS) {
      result = VK_ERROR_OUT_OF_DATE_KHR;
   }
   pthread_mutex_unlock(&session->lock);
   return result;
}

struct wsi_kolos_pool_info
wsi_kolos_pool_get_info(const struct wsi_kolos_pool *pool)
{
   return pool ? pool->info : (struct wsi_kolos_pool_info){0};
}

const struct wsi_kolos_image *
wsi_kolos_pool_get_image(const struct wsi_kolos_pool *pool, uint32_t slot)
{
   return pool && slot < pool->info.slot_count &&
          pool->images[slot].base.image != VK_NULL_HANDLE
      ? &pool->images[slot] : NULL;
}

void
wsi_kolos_unpin_pool(struct wsi_kolos_pool *pool)
{
   if (!pool)
      return;
   struct wsi_kolos_session *session = pool->session;
   pthread_mutex_lock(&session->lock);
   if (pool->pins) {
      pool->pins--;
      collect_retired(session);
   }
   pthread_mutex_unlock(&session->lock);
}

VkResult
wsi_kolos_pool_set_available(struct wsi_kolos_session *session,
                             struct wsi_kolos_identity id)
{
   if (!session)
      return VK_ERROR_INITIALIZATION_FAILED;
   pthread_mutex_lock(&session->lock);
   VkResult result = session->error;
   struct wsi_kolos_pool *pool = find_pool(session, id.generation);
   if (result == VK_SUCCESS &&
       (session->destroying || id.session_id != session->id || !pool ||
        id.slot >= pool->info.slot_count ||
        pool->images[id.slot].base.image == VK_NULL_HANDLE ||
        (pool->state[id.slot] != KOLOS_WAIT_FREE &&
         pool->state[id.slot] != KOLOS_PRESENTED)))
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
   if (result == VK_SUCCESS) {
      pool->state[id.slot] = KOLOS_AVAILABLE;
      collect_retired(session);
      pthread_cond_broadcast(&session->changed);
   }
   pthread_mutex_unlock(&session->lock);
   return result;
}

void
wsi_kolos_session_error(struct wsi_kolos_session *session, VkResult error)
{
   if (!session)
      return;
   pthread_mutex_lock(&session->lock);
   set_error_locked(session, error);
   pthread_mutex_unlock(&session->lock);
}

static struct timespec
deadline_from_now(uint64_t timeout_ns)
{
   struct timespec when;
   clock_gettime(CLOCK_MONOTONIC, &when);
   uint64_t seconds = timeout_ns / 1000000000ull;
   uint64_t nanos = timeout_ns % 1000000000ull;
   if (seconds > (uint64_t)INT64_MAX - (uint64_t)when.tv_sec - 1)
      seconds = (uint64_t)INT64_MAX - (uint64_t)when.tv_sec - 1;
   when.tv_sec += (time_t)seconds;
   when.tv_nsec += (long)nanos;
   if (when.tv_nsec >= 1000000000L) {
      when.tv_sec++;
      when.tv_nsec -= 1000000000L;
   }
   return when;
}

VkResult
wsi_kolos_pool_acquire(struct wsi_kolos_session *session, uint64_t timeout_ns,
                       struct wsi_kolos_image **out)
{
   if (!session || !out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   struct timespec deadline = deadline_from_now(timeout_ns);
   pthread_mutex_lock(&session->lock);
   VkResult result;
   for (;;) {
      if (session->error != VK_SUCCESS || session->destroying) {
         result = session->error != VK_SUCCESS ? session->error :
                  VK_ERROR_SURFACE_LOST_KHR;
         break;
      }
      if (session->current) {
         for (uint32_t i = 0; i < session->current->info.slot_count; i++) {
            if (session->current->state[i] == KOLOS_AVAILABLE) {
               session->current->state[i] = KOLOS_ACQUIRED;
               session->current->leases++;
               *out = &session->current->images[i];
               result = VK_SUCCESS;
               goto out;
            }
         }
      }
      if (session->retired_without_current) {
         result = VK_ERROR_OUT_OF_DATE_KHR;
         break;
      }
      if (!timeout_ns) {
         result = VK_NOT_READY;
         break;
      }
      int waited = timeout_ns == UINT64_MAX
         ? pthread_cond_wait(&session->changed, &session->lock)
         : pthread_cond_timedwait(&session->changed, &session->lock, &deadline);
      if (waited == ETIMEDOUT) {
         result = VK_TIMEOUT;
         break;
      }
      if (waited != 0) {
         result = VK_ERROR_SURFACE_LOST_KHR;
         break;
      }
   }
out:
   pthread_mutex_unlock(&session->lock);
   return result;
}

VkResult
wsi_kolos_pool_try_acquire(struct wsi_kolos_pool *pool,
                           struct wsi_kolos_image **out)
{
   if (!pool || !out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   struct wsi_kolos_session *session = pool->session;
   pthread_mutex_lock(&session->lock);
   VkResult result = session->error;
   if (result == VK_SUCCESS && session->destroying)
      result = VK_ERROR_SURFACE_LOST_KHR;
   if (result == VK_SUCCESS && pool != session->current)
      result = VK_ERROR_OUT_OF_DATE_KHR;
   if (result == VK_SUCCESS) {
      result = VK_NOT_READY;
      for (uint32_t i = 0; i < pool->info.slot_count; i++) {
         if (pool->state[i] == KOLOS_AVAILABLE) {
            pool->state[i] = KOLOS_ACQUIRED;
            pool->leases++;
            *out = &pool->images[i];
            result = VK_SUCCESS;
            break;
         }
      }
   }
   pthread_mutex_unlock(&session->lock);
   return result;
}

VkResult
wsi_kolos_pool_presented(struct wsi_kolos_session *session,
                         struct wsi_kolos_image *image, uint64_t serial)
{
   if (!session || !image)
      return VK_ERROR_INITIALIZATION_FAILED;
   pthread_mutex_lock(&session->lock);
   struct wsi_kolos_pool *pool = find_pool(session, image->id.generation);
   if (!pool || image->id.session_id != session->id ||
       image->id.slot >= pool->info.slot_count ||
       &pool->images[image->id.slot] != image ||
       pool->state[image->id.slot] != KOLOS_ACQUIRED) {
      pthread_mutex_unlock(&session->lock);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   if (pool != session->current) {
      pool->state[image->id.slot] = KOLOS_AVAILABLE;
      pool->leases--;
      collect_retired(session);
      pthread_mutex_unlock(&session->lock);
      return VK_ERROR_OUT_OF_DATE_KHR;
   }
   if (session->error != VK_SUCCESS || session->destroying ||
       serial != session->next_serial) {
      VkResult result = session->error != VK_SUCCESS ? session->error :
                        VK_ERROR_INVALID_EXTERNAL_HANDLE;
      pthread_mutex_unlock(&session->lock);
      return result;
   }
   /* The callback only enqueues a transport message.  It must not wait for
    * FREE or synchronously dispatch back into this session. */
   VkResult result = session->transport.present(
      session->transport_data, image->id, serial);
   pool->state[image->id.slot] = KOLOS_PRESENTED;
   pool->leases--;
   if (result == VK_SUCCESS)
      session->next_serial++;
   else
      set_error_locked(session, result);
   pthread_mutex_unlock(&session->lock);
   return result;
}

void
wsi_kolos_release_image(struct wsi_kolos_session *session,
                         struct wsi_kolos_image *image)
{
   if (!session || !image)
      return;
   pthread_mutex_lock(&session->lock);
   struct wsi_kolos_pool *pool = find_pool(session, image->id.generation);
   if (pool && image->id.session_id == session->id &&
       image->id.slot < pool->info.slot_count &&
       &pool->images[image->id.slot] == image &&
       pool->state[image->id.slot] == KOLOS_ACQUIRED) {
      pool->state[image->id.slot] = KOLOS_AVAILABLE;
      pool->leases--;
      collect_retired(session);
      pthread_cond_broadcast(&session->changed);
   }
   pthread_mutex_unlock(&session->lock);
}

void
wsi_kolos_pool_retire(struct wsi_kolos_session *session)
{
   if (!session)
      return;
   pthread_mutex_lock(&session->lock);
   if (session->current) {
      session->current->next = session->retired;
      session->retired = session->current;
      session->current = NULL;
      session->retired_without_current = true;
      collect_retired(session);
      pthread_cond_broadcast(&session->changed);
   }
   pthread_mutex_unlock(&session->lock);
}

void
wsi_kolos_session_destroy(struct wsi_kolos_session *session)
{
   if (!session)
      return;
   /* Caller stops transport event delivery and returns all leases/pins first.
    * No reader thread or FD is owned by this transport-neutral core. */
   pthread_mutex_lock(&session->lock);
   session->destroying = true;
   pthread_cond_broadcast(&session->changed);
   pthread_mutex_unlock(&session->lock);
   destroy_pool(session->staging);
   destroy_pool(session->current);
   while (session->retired) {
      struct wsi_kolos_pool *next = session->retired->next;
      destroy_pool(session->retired);
      session->retired = next;
   }
   pthread_cond_destroy(&session->changed);
   pthread_mutex_destroy(&session->lock);
   free(session);
}
