/* Copyright © 2026 Kolos contributors
 * SPDX-License-Identifier: MIT
 *
 * Graphics-v2 is a little-endian byte protocol.  This module is intentionally
 * independent of wl_surface and of any particular application render layout.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "wsi_common_kolos.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define KGX_MAGIC 0x3158474bu
#define KGX_VERSION 2u
#define KGX_PAYLOAD_MAX 4096u
#define KGX_SLOTS 3u
#define KGX_HELLO 1u
#define KGX_HELLO_ACK 2u
#define KGX_SESSION_INFO 3u
#define KGX_GET_BUFFER 4u
#define KGX_BUFFER_INFO 5u
#define KGX_IMPORT_ACK 6u
#define KGX_START 7u
#define KGX_FREE 8u
#define KGX_PRESENT 9u
#define KGX_ACK 10u
#define KGX_STOP 11u
#define KGX_DONE 12u
#define KGX_ERROR 13u
#define KGX_POOL_REPLACED 14u
#define KGX_POOL_READY 15u

enum kolos_slot_state {
   KOLOS_WAIT_FREE,
   KOLOS_AVAILABLE,
   KOLOS_ACQUIRED,
   KOLOS_PRESENTED,
};

struct wsi_kolos_pool {
   struct wsi_kolos_pool *next;
   uint64_t generation;
   uint32_t width, height, count;
   uint32_t leases, pins;
   struct wsi_kolos_image images[KGX_SLOTS];
   enum kolos_slot_state state[KGX_SLOTS];
};

struct kolos_session {
   uint64_t id;
   struct wsi_kolos_pool *current, *retired;
};

struct wsi_kolos_connection {
   const struct wsi_device *wsi;
   VkDevice device;
   PFN_vkGetMemoryFdPropertiesKHR get_fd_properties;
   VkImageUsageFlags usage;
   VkAllocationCallbacks allocator;
   bool has_allocator;
   char *path;
   int fd, import_fd;
   struct kolos_session session;
   uint64_t next_serial;
   pthread_mutex_t lock, write_lock;
   pthread_cond_t changed;
   pthread_t reader;
   bool reader_started, closing, stopping, done, pending, acked;
   uint64_t pending_generation, pending_serial;
   uint32_t pending_slot;
   VkResult failure;
};

struct kolos_frame {
   uint32_t opcode, length;
   uint8_t payload[KGX_PAYLOAD_MAX];
};

static uint32_t
get32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
get64(const uint8_t *p)
{
   return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void
put32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; i++)
      p[i] = (uint8_t)(v >> (8 * i));
}

static void
put64(uint8_t *p, uint64_t v)
{
   put32(p, (uint32_t)v);
   put32(p + 4, (uint32_t)(v >> 32));
}

static void
put_identity(uint8_t *p, uint64_t session, uint64_t generation, uint32_t slot)
{
   put64(p, session);
   put64(p + 8, generation);
   put32(p + 16, slot);
}

static bool
wait_io(int fd, short events, int timeout_ms)
{
   struct pollfd p = { .fd = fd, .events = events };
   int n;
   do {
      n = poll(&p, 1, timeout_ms);
   } while (n < 0 && errno == EINTR);
   return n > 0 && (p.revents & events) != 0;
}

static bool
read_full(int fd, uint8_t *out, size_t length, int timeout_ms)
{
   while (length) {
      if (!wait_io(fd, POLLIN, timeout_ms))
         return false;
      ssize_t n = recv(fd, out, length, MSG_DONTWAIT);
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
         continue;
      if (n <= 0)
         return false;
      out += n;
      length -= (size_t)n;
   }
   return true;
}

static bool
write_full(int fd, const uint8_t *data, size_t length)
{
   while (length) {
      if (!wait_io(fd, POLLOUT, 30000))
         return false;
      ssize_t n = send(fd, data, length, MSG_DONTWAIT | MSG_NOSIGNAL);
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
         continue;
      if (n <= 0)
         return false;
      data += n;
      length -= (size_t)n;
   }
   return true;
}

static bool
read_frame(int fd, struct kolos_frame *frame, int timeout_ms)
{
   uint8_t header[16];
   if (!read_full(fd, header, sizeof(header), timeout_ms) ||
       get32(header) != KGX_MAGIC || get32(header + 4) != KGX_VERSION)
      return false;
   frame->opcode = get32(header + 8);
   frame->length = get32(header + 12);
   return frame->length <= KGX_PAYLOAD_MAX &&
          read_full(fd, frame->payload, frame->length, timeout_ms);
}

static bool
write_frame(int fd, uint32_t opcode, const uint8_t *payload, uint32_t length)
{
   if (length > KGX_PAYLOAD_MAX)
      return false;
   uint8_t header[16];
   put32(header, KGX_MAGIC);
   put32(header + 4, KGX_VERSION);
   put32(header + 8, opcode);
   put32(header + 12, length);
   return write_full(fd, header, sizeof(header)) &&
          (!length || write_full(fd, payload, length));
}

static bool
send_control(struct wsi_kolos_connection *c, uint32_t opcode,
             const uint8_t *payload, uint32_t length)
{
   pthread_mutex_lock(&c->write_lock);
   bool ok = write_frame(c->fd, opcode, payload, length);
   pthread_mutex_unlock(&c->write_lock);
   return ok;
}

static int
connect_socket(const char *path)
{
   struct sockaddr_un address = { .sun_family = AF_UNIX };
   size_t length = strlen(path);
   if (!length || length >= sizeof(address.sun_path))
      return -1;
   memcpy(address.sun_path, path, length + 1);
   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (fd < 0)
      return -1;
   if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
      close(fd);
      return -1;
   }
   return fd;
}

static void
destroy_image(struct wsi_kolos_connection *c, struct wsi_kolos_image *image)
{
   const VkAllocationCallbacks *allocator = c->has_allocator ? &c->allocator : NULL;
   if (image->base.image != VK_NULL_HANDLE)
      c->wsi->DestroyImage(c->device, image->base.image, allocator);
   if (image->base.memory != VK_NULL_HANDLE)
      c->wsi->FreeMemory(c->device, image->base.memory, allocator);
   image->base.image = VK_NULL_HANDLE;
   image->base.memory = VK_NULL_HANDLE;
}

static void
destroy_pool(struct wsi_kolos_connection *c, struct wsi_kolos_pool *pool)
{
   if (!pool)
      return;
   for (uint32_t i = 0; i < pool->count; i++)
      destroy_image(c, &pool->images[i]);
   free(pool);
}

static void
close_fds(int fds[2])
{
   for (unsigned i = 0; i < 2; i++) {
      if (fds[i] >= 0) {
         close(fds[i]);
         fds[i] = -1;
      }
   }
}

/* Android sends native_handle bytes followed by SCM_RIGHTS on the auxiliary
 * GET_BUFFER connection, then half-closes its write side. */
static bool
receive_handle(int fd, uint8_t bytes[148], int fds[2])
{
   size_t used = 0;
   unsigned fd_count = 0;
   for (;;) {
      if (!wait_io(fd, POLLIN, 30000))
         return false;
      uint8_t data[4096];
      union {
         struct cmsghdr align;
         uint8_t bytes[CMSG_SPACE(64 * sizeof(int))];
      } control;
      struct iovec iov = { .iov_base = data, .iov_len = sizeof(data) };
      struct msghdr message = {
         .msg_iov = &iov, .msg_iovlen = 1,
         .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
      };
      ssize_t n = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
         continue;
      if (n < 0)
         return false;
      bool valid = !(message.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
      for (struct cmsghdr *entry = CMSG_FIRSTHDR(&message); entry;
           entry = CMSG_NXTHDR(&message, entry)) {
         if (entry->cmsg_level != SOL_SOCKET || entry->cmsg_type != SCM_RIGHTS)
            continue;
         if (entry->cmsg_len < CMSG_LEN(0))
            return false;
         size_t rights_length = entry->cmsg_len - CMSG_LEN(0);
         if (rights_length % sizeof(int) != 0)
            valid = false;
         size_t count = rights_length / sizeof(int);
         for (size_t i = 0; i < count; i++) {
            int received;
            memcpy(&received, (uint8_t *)CMSG_DATA(entry) + i * sizeof(int),
                   sizeof(int));
            if (fd_count < 2)
               fds[fd_count++] = received;
            else {
               close(received);
               valid = false;
            }
         }
      }
      if (!valid || used + (size_t)n > 148)
         return false;
      if (n == 0)
         return used == 148 && fd_count == 2;
      memcpy(bytes + used, data, (size_t)n);
      used += (size_t)n;
   }
}

/* Device-specific Qualcomm compatibility path from KolosLinux.  The native
 * handle is NOT a Mesa ABI: every observed field, allocation size and FD
 * must match before the one-plane linear import is attempted. */
static bool
qualcomm_linear_layout(const uint8_t bytes[148], int pixel_fd,
                       uint32_t width, uint32_t height, uint32_t stride,
                       uint64_t *allocation_size)
{
   uint32_t w[37];
   for (unsigned i = 0; i < 37; i++)
      w[i] = get32(bytes + i * 4);
   if (stride > UINT32_MAX / 4 || w[16] < height ||
       (uint64_t)stride * w[16] * 4 > UINT32_MAX ||
       (uint64_t)stride * height * 4 > UINT32_MAX)
      return false;
   if (w[0] != 0x47423031 || w[1] != width || w[2] != height ||
       w[3] != stride || w[4] != 1 || w[5] != 1 || w[6] != 0xb00 ||
       w[10] != 2 || w[11] != 24 || w[13] != 0x676d736d ||
       w[14] != 0x00100288 || w[15] != stride || w[17] != width ||
       w[18] != height || w[19] != 1 || w[20] != 0 || w[21] != 1 ||
       w[24] != 0xb00 || w[25] != 0 || w[27] != 0 || w[28] != 0)
      return false;
   uint64_t bytes_expected = (uint64_t)stride * w[16] * 4;
   struct stat statbuf;
   if (w[26] != bytes_expected || fstat(pixel_fd, &statbuf) != 0 ||
       statbuf.st_size < 0 || (uint64_t)statbuf.st_size != bytes_expected)
      return false;
   *allocation_size = bytes_expected;
   return true;
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
wsi_kolos_import_image(struct wsi_kolos_connection *c,
                       struct wsi_kolos_image *image, int pixel_fd,
                       const uint8_t handle[148])
{
   const struct wsi_device *wsi = c->wsi;
   if (!qualcomm_linear_layout(handle, pixel_fd, image->width, image->height,
                               image->stride, &image->allocation_size))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
   const VkExternalMemoryHandleTypeFlagBits handle_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   VkDrmFormatModifierPropertiesListEXT modifiers = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
   };
   VkFormatProperties2 format_props = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &modifiers,
   };
   if (!wsi->GetPhysicalDeviceFormatProperties2 ||
       !wsi->GetPhysicalDeviceImageFormatProperties2 ||
       !wsi->CreateImage || !wsi->GetImageMemoryRequirements ||
       !wsi->AllocateMemory || !wsi->BindImageMemory ||
       !wsi->DestroyImage || !wsi->FreeMemory || !c->get_fd_properties)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   wsi->GetPhysicalDeviceFormatProperties2(wsi->pdevice, format, &format_props);
   uint32_t count = modifiers.drmFormatModifierCount;
   if (!count || count > 1024)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkDrmFormatModifierPropertiesEXT *list = calloc(count, sizeof(*list));
   if (!list)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   modifiers.pDrmFormatModifierProperties = list;
   wsi->GetPhysicalDeviceFormatProperties2(wsi->pdevice, format, &format_props);
   bool supported = modifiers.drmFormatModifierCount <= count;
   VkFormatFeatureFlags needed = required_features(c->usage);
   for (uint32_t i = 0; supported && i < modifiers.drmFormatModifierCount; i++) {
      if (list[i].drmFormatModifier == 0 &&
          list[i].drmFormatModifierPlaneCount == 1 &&
          (list[i].drmFormatModifierTilingFeatures & needed) == needed) {
         supported = true;
         goto modifier_found;
      }
   }
   supported = false;
modifier_found:
   free(list);
   if (!supported)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
      .drmFormatModifier = 0,
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
      .usage = c->usage,
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
       properties.imageFormatProperties.maxExtent.width < image->width ||
       properties.imageFormatProperties.maxExtent.height < image->height)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   VkSubresourceLayout layout = {
      .offset = 0, .rowPitch = (VkDeviceSize)image->stride * 4,
   };
   VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_mod = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = 0, .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &layout,
   };
   VkExternalMemoryImageCreateInfo external_image = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &explicit_mod, .handleTypes = handle_type,
   };
   VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external_image, .imageType = VK_IMAGE_TYPE_2D,
      .format = format, .extent = { image->width, image->height, 1 },
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = c->usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   const VkAllocationCallbacks *allocator = c->has_allocator ? &c->allocator : NULL;
   result = wsi->CreateImage(c->device, &image_info, allocator, &image->base.image);
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements req;
   wsi->GetImageMemoryRequirements(c->device, image->base.image, &req);
   if (req.size > image->allocation_size)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   result = c->get_fd_properties(c->device, handle_type, pixel_fd, &fd_props);
   if (result != VK_SUCCESS)
      return result;
   uint32_t bits = req.memoryTypeBits & fd_props.memoryTypeBits;
   if (!bits)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   uint32_t memory_type = 0;
   while (!(bits & (1u << memory_type)))
      memory_type++;
   int import_fd = fcntl(pixel_fd, F_DUPFD_CLOEXEC, 0);
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
   result = wsi->AllocateMemory(c->device, &alloc, allocator, &image->base.memory);
   if (result != VK_SUCCESS) {
      close(import_fd); /* Vulkan takes ownership only after success. */
      return result;
   }
   result = wsi->BindImageMemory(c->device, image->base.image,
                                 image->base.memory, 0);
   if (result != VK_SUCCESS)
      return result;
   image->base.dma_buf_fd = -1; /* Original SCM_RIGHTS FD closes below. */
   image->base.drm_modifier = 0;
   image->base.num_planes = 1;
   image->base.sizes[0] = (uint32_t)image->allocation_size;
   image->base.offsets[0] = 0;
   image->base.row_pitches[0] = image->stride * 4;
   return VK_SUCCESS;
}

static VkResult
import_pool(struct wsi_kolos_connection *c, uint64_t generation,
            uint32_t width, uint32_t height, uint32_t format, uint32_t count,
            struct wsi_kolos_pool **out)
{
   if (!generation || !width || !height || format != 1 ||
       count != KGX_SLOTS || width > UINT32_MAX / 4 || !c->usage)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   struct wsi_kolos_pool *pool = calloc(1, sizeof(*pool));
   if (!pool)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   pool->generation = generation;
   pool->width = width;
   pool->height = height;
   pool->count = count;
   VkResult result = VK_ERROR_SURFACE_LOST_KHR;
   for (uint32_t slot = 0; slot < count; slot++) {
      result = VK_ERROR_SURFACE_LOST_KHR;
      int fd = connect_socket(c->path);
      if (fd < 0)
         goto fail;
      pthread_mutex_lock(&c->lock);
      c->import_fd = fd;
      bool closing = c->closing;
      pthread_mutex_unlock(&c->lock);
      if (closing) {
         pthread_mutex_lock(&c->lock);
         c->import_fd = -1;
         pthread_mutex_unlock(&c->lock);
         close(fd);
         goto fail;
      }
      uint8_t identity[20];
      put_identity(identity, c->session.id, generation, slot);
      struct kolos_frame reply;
      int fds[2] = { -1, -1 };
      uint8_t handle[148];
      bool ok = write_frame(fd, KGX_GET_BUFFER, identity, sizeof(identity)) &&
                read_frame(fd, &reply, 30000) &&
                reply.opcode == KGX_BUFFER_INFO && reply.length == 40 &&
                get64(reply.payload) == c->session.id &&
                get64(reply.payload + 8) == generation &&
                get32(reply.payload + 16) == slot &&
                get32(reply.payload + 20) == width &&
                get32(reply.payload + 24) == height &&
                get32(reply.payload + 28) == format &&
                get32(reply.payload + 32) == 1;
      uint32_t stride = ok ? get32(reply.payload + 36) : 0;
      ok = ok && stride >= width && receive_handle(fd, handle, fds);
      if (ok) {
         struct wsi_kolos_image *image = &pool->images[slot];
         image->session_id = c->session.id;
         image->generation = generation;
         image->slot = slot;
         image->width = width;
         image->height = height;
         image->stride = stride;
         result = wsi_kolos_import_image(c, image, fds[0], handle);
         ok = result == VK_SUCCESS &&
              write_frame(fd, KGX_IMPORT_ACK, identity, sizeof(identity));
         if (result == VK_SUCCESS && !ok)
            result = VK_ERROR_SURFACE_LOST_KHR;
      }
      close_fds(fds);
      pthread_mutex_lock(&c->lock);
      c->import_fd = -1;
      pthread_mutex_unlock(&c->lock);
      close(fd);
      if (!ok)
         goto fail;
   }
   *out = pool;
   return VK_SUCCESS;
fail:
   destroy_pool(c, pool);
   return result;
}

static struct wsi_kolos_pool *
find_pool(struct wsi_kolos_connection *c, uint64_t generation)
{
   if (c->session.current && c->session.current->generation == generation)
      return c->session.current;
   for (struct wsi_kolos_pool *pool = c->session.retired; pool; pool = pool->next)
      if (pool->generation == generation)
         return pool;
   return NULL;
}

static void collect_retired(struct wsi_kolos_connection *c);

VkResult
wsi_kolos_pin_current_pool(struct wsi_kolos_connection *c,
                           struct wsi_kolos_pool **out)
{
   if (!c || !out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   pthread_mutex_lock(&c->lock);
   VkResult result = c->failure;
   if (result == VK_SUCCESS && !c->closing && !c->stopping && c->session.current) {
      c->session.current->pins++;
      *out = c->session.current;
   } else if (result == VK_SUCCESS) {
      result = VK_ERROR_SURFACE_LOST_KHR;
   }
   pthread_mutex_unlock(&c->lock);
   return result;
}

struct wsi_kolos_pool_info
wsi_kolos_pool_get_info(const struct wsi_kolos_pool *pool)
{
   if (!pool)
      return (struct wsi_kolos_pool_info){0};
   return (struct wsi_kolos_pool_info){
      .session_id = pool->images[0].session_id,
      .generation = pool->generation,
      .width = pool->width, .height = pool->height,
      .slot_count = pool->count,
   };
}

const struct wsi_kolos_image *
wsi_kolos_pool_get_image(const struct wsi_kolos_pool *pool, uint32_t slot)
{
   return pool && slot < pool->count ? &pool->images[slot] : NULL;
}

void
wsi_kolos_unpin_pool(struct wsi_kolos_connection *c, struct wsi_kolos_pool *pool)
{
   if (!c || !pool)
      return;
   pthread_mutex_lock(&c->lock);
   if (find_pool(c, pool->generation) == pool && pool->pins) {
      pool->pins--;
      collect_retired(c);
   }
   pthread_mutex_unlock(&c->lock);
}

static void
collect_retired(struct wsi_kolos_connection *c)
{
   struct wsi_kolos_pool **link = &c->session.retired;
   while (*link) {
      struct wsi_kolos_pool *pool = *link;
      bool busy = pool->leases != 0 || pool->pins != 0;
      for (uint32_t i = 0; i < pool->count; i++)
         busy |= pool->state[i] != KOLOS_AVAILABLE;
      if (busy) {
         link = &pool->next;
      } else {
         *link = pool->next;
         destroy_pool(c, pool);
      }
   }
}

static VkResult
dispatch_frame(struct wsi_kolos_connection *c, const struct kolos_frame *f)
{
   if (f->opcode == KGX_POOL_REPLACED) {
      if (f->length != 32 || get64(f->payload) != c->session.id)
         return VK_ERROR_SURFACE_LOST_KHR;
      uint64_t generation = get64(f->payload + 8);
      pthread_mutex_lock(&c->lock);
      bool valid = c->session.current && generation > c->session.current->generation;
      pthread_mutex_unlock(&c->lock);
      if (!valid)
         return VK_ERROR_SURFACE_LOST_KHR;
      struct wsi_kolos_pool *next = NULL;
      VkResult result = import_pool(c, generation, get32(f->payload + 16),
                                    get32(f->payload + 20),
                                    get32(f->payload + 24),
                                    get32(f->payload + 28), &next);
      if (result != VK_SUCCESS)
         return result;
      uint8_t ready[16];
      put64(ready, c->session.id);
      put64(ready + 8, generation);
      if (!send_control(c, KGX_POOL_READY, ready, sizeof(ready))) {
         destroy_pool(c, next);
         return VK_ERROR_SURFACE_LOST_KHR;
      }
      pthread_mutex_lock(&c->lock);
      struct wsi_kolos_pool *old = c->session.current;
      old->next = c->session.retired;
      c->session.retired = old;
      c->session.current = next;
      collect_retired(c);
      pthread_cond_broadcast(&c->changed);
      pthread_mutex_unlock(&c->lock);
      return VK_SUCCESS;
   }
   pthread_mutex_lock(&c->lock);
   VkResult result = VK_SUCCESS;
   switch (f->opcode) {
   case KGX_FREE: {
      if (f->length != 20 || get64(f->payload) != c->session.id) {
         result = VK_ERROR_SURFACE_LOST_KHR;
         break;
      }
      struct wsi_kolos_pool *pool = find_pool(c, get64(f->payload + 8));
      uint32_t slot = get32(f->payload + 16);
      if (!pool || slot >= pool->count ||
          (pool->state[slot] != KOLOS_WAIT_FREE &&
           pool->state[slot] != KOLOS_PRESENTED)) {
         result = VK_ERROR_SURFACE_LOST_KHR;
         break;
      }
      pool->state[slot] = KOLOS_AVAILABLE;
      collect_retired(c);
      pthread_cond_broadcast(&c->changed);
      break;
   }
   case KGX_ACK:
      if (f->length != 28 || !c->pending ||
          get64(f->payload) != c->session.id ||
          get64(f->payload + 8) != c->pending_generation ||
          get32(f->payload + 16) != c->pending_slot ||
          get64(f->payload + 20) != c->pending_serial) {
         result = VK_ERROR_SURFACE_LOST_KHR;
         break;
      }
      c->acked = true;
      pthread_cond_broadcast(&c->changed);
      break;
   case KGX_DONE:
      if (f->length != 24 || get64(f->payload) != c->session.id ||
          !c->stopping || !find_pool(c, get64(f->payload + 8)) ||
          get64(f->payload + 16) != c->next_serial) {
         result = VK_ERROR_SURFACE_LOST_KHR;
         break;
      }
      c->done = true;
      pthread_cond_broadcast(&c->changed);
      break;
   case KGX_ERROR:
      result = VK_ERROR_SURFACE_LOST_KHR;
      break;
   default:
      result = VK_ERROR_SURFACE_LOST_KHR;
      break;
   }
   pthread_mutex_unlock(&c->lock);
   return result;
}

static void *
reader_main(void *data)
{
   struct wsi_kolos_connection *c = data;
   struct kolos_frame frame;
   VkResult result = VK_SUCCESS;
   while (read_frame(c->fd, &frame, -1)) {
      result = dispatch_frame(c, &frame);
      if (result != VK_SUCCESS || frame.opcode == KGX_DONE)
         break;
   }
   pthread_mutex_lock(&c->lock);
   if (!c->closing && !c->done)
      c->failure = result == VK_SUCCESS ? VK_ERROR_SURFACE_LOST_KHR : result;
   pthread_cond_broadcast(&c->changed);
   pthread_mutex_unlock(&c->lock);
   return NULL;
}

VkResult
wsi_kolos_connect(const char *socket_path, const struct wsi_device *wsi,
                  VkDevice device,
                  PFN_vkGetMemoryFdPropertiesKHR get_fd_properties,
                  VkImageUsageFlags usage,
                  const VkAllocationCallbacks *allocator,
                  struct wsi_kolos_connection **out)
{
   if (!socket_path || !wsi || !get_fd_properties || !usage || !out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   struct wsi_kolos_connection *c = calloc(1, sizeof(*c));
   if (!c)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   c->fd = -1;
   c->import_fd = -1;
   c->wsi = wsi;
   c->device = device;
   c->get_fd_properties = get_fd_properties;
   c->usage = usage;
   if (allocator) {
      c->allocator = *allocator;
      c->has_allocator = true;
   }
   c->path = strdup(socket_path);
   if (!c->path) {
      free(c);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   pthread_mutex_init(&c->lock, NULL);
   pthread_mutex_init(&c->write_lock, NULL);
   pthread_condattr_t attr;
   pthread_condattr_init(&attr);
   pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
   pthread_cond_init(&c->changed, &attr);
   pthread_condattr_destroy(&attr);
   VkResult result = VK_ERROR_SURFACE_LOST_KHR;
   c->fd = connect_socket(socket_path);
   if (c->fd < 0)
      goto fail;
   struct kolos_frame frame;
   if (!send_control(c, KGX_HELLO, NULL, 0) ||
       !read_frame(c->fd, &frame, 30000) ||
       frame.opcode != KGX_HELLO_ACK || frame.length != 0 ||
       !read_frame(c->fd, &frame, 30000) ||
       frame.opcode != KGX_SESSION_INFO || frame.length != 32)
      goto fail;
   c->session.id = get64(frame.payload);
   uint64_t generation = get64(frame.payload + 8);
   if (!c->session.id)
      goto fail;
   result = import_pool(c, generation, get32(frame.payload + 16),
                        get32(frame.payload + 20), get32(frame.payload + 24),
                        get32(frame.payload + 28), &c->session.current);
   if (result != VK_SUCCESS)
      goto fail;
   uint8_t start[16];
   put64(start, c->session.id);
   put64(start + 8, generation);
   if (!send_control(c, KGX_START, start, sizeof(start))) {
      result = VK_ERROR_SURFACE_LOST_KHR;
      goto fail;
   }
   if (pthread_create(&c->reader, NULL, reader_main, c) != 0) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }
   c->reader_started = true;
   *out = c;
   return VK_SUCCESS;
fail:
   wsi_kolos_destroy(c);
   return result;
}

static struct timespec
deadline_from_now(uint64_t timeout_ns)
{
   struct timespec when;
   clock_gettime(CLOCK_MONOTONIC, &when);
   uint64_t sec = timeout_ns / 1000000000ull;
   uint64_t nsec = timeout_ns % 1000000000ull;
   if (sec > (uint64_t)INT64_MAX - (uint64_t)when.tv_sec - 1)
      sec = (uint64_t)INT64_MAX - (uint64_t)when.tv_sec - 1;
   when.tv_sec += (time_t)sec;
   when.tv_nsec += (long)nsec;
   if (when.tv_nsec >= 1000000000L) {
      when.tv_sec++;
      when.tv_nsec -= 1000000000L;
   }
   return when;
}

VkResult
wsi_kolos_wait_available(struct wsi_kolos_connection *c, uint64_t timeout_ns,
                         struct wsi_kolos_image **out)
{
   if (!c || !out)
      return VK_ERROR_INITIALIZATION_FAILED;
   *out = NULL;
   struct timespec deadline = deadline_from_now(timeout_ns);
   pthread_mutex_lock(&c->lock);
   VkResult result;
   for (;;) {
      if (c->failure != VK_SUCCESS || c->closing || c->stopping || c->done) {
         result = c->failure != VK_SUCCESS ? c->failure : VK_ERROR_SURFACE_LOST_KHR;
         break;
      }
      for (uint32_t i = 0; i < c->session.current->count; i++) {
         if (c->session.current->state[i] == KOLOS_AVAILABLE) {
            c->session.current->state[i] = KOLOS_ACQUIRED;
            c->session.current->leases++;
            *out = &c->session.current->images[i];
            result = VK_SUCCESS;
            goto out;
         }
      }
      if (!timeout_ns) {
         result = VK_NOT_READY;
         break;
      }
      int waited = timeout_ns == UINT64_MAX
         ? pthread_cond_wait(&c->changed, &c->lock)
         : pthread_cond_timedwait(&c->changed, &c->lock, &deadline);
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
   pthread_mutex_unlock(&c->lock);
   return result;
}

VkResult
wsi_kolos_present(struct wsi_kolos_connection *c,
                  struct wsi_kolos_image *image, uint64_t serial)
{
   if (!c || !image)
      return VK_ERROR_INITIALIZATION_FAILED;
   pthread_mutex_lock(&c->lock);
   struct wsi_kolos_pool *pool = find_pool(c, image->generation);
   if (!pool || image->session_id != c->session.id ||
       image->slot >= pool->count || &pool->images[image->slot] != image ||
       pool->state[image->slot] != KOLOS_ACQUIRED) {
      pthread_mutex_unlock(&c->lock);
      return VK_ERROR_VALIDATION_FAILED_EXT;
   }
   while (c->pending && c->failure == VK_SUCCESS && !c->closing)
      pthread_cond_wait(&c->changed, &c->lock);
   if (c->failure != VK_SUCCESS || c->closing || c->stopping) {
      VkResult result = c->failure != VK_SUCCESS ? c->failure : VK_ERROR_SURFACE_LOST_KHR;
      pthread_mutex_unlock(&c->lock);
      return result;
   }
   if (pool != c->session.current) {
      pool->state[image->slot] = KOLOS_AVAILABLE;
      pool->leases--;
      collect_retired(c);
      pthread_mutex_unlock(&c->lock);
      return VK_ERROR_OUT_OF_DATE_KHR;
   }
   if (serial != c->next_serial) {
      pthread_mutex_unlock(&c->lock);
      return VK_ERROR_VALIDATION_FAILED_EXT;
   }
   c->pending = true;
   c->acked = false;
   c->pending_generation = image->generation;
   c->pending_slot = image->slot;
   c->pending_serial = serial;
   pool->state[image->slot] = KOLOS_PRESENTED;
   uint8_t payload[28];
   put_identity(payload, c->session.id, image->generation, image->slot);
   put64(payload + 20, serial);
   bool sent = send_control(c, KGX_PRESENT, payload, sizeof(payload));
   if (!sent) {
      c->failure = VK_ERROR_SURFACE_LOST_KHR;
      pthread_cond_broadcast(&c->changed);
   }
   while (sent && !c->acked && c->session.current == pool &&
          c->failure == VK_SUCCESS && !c->closing)
      pthread_cond_wait(&c->changed, &c->lock);
   VkResult result;
   if (c->acked) {
      c->next_serial++;
      result = VK_SUCCESS;
   } else if (c->failure != VK_SUCCESS || c->closing) {
      result = c->failure != VK_SUCCESS ? c->failure : VK_ERROR_SURFACE_LOST_KHR;
   } else {
      /* Android explicitly drops a pre-replacement in-flight PRESENT. */
      pool->state[image->slot] = KOLOS_AVAILABLE;
      result = VK_ERROR_OUT_OF_DATE_KHR;
   }
   pool->leases--;
   c->pending = false;
   c->acked = false;
   collect_retired(c);
   pthread_cond_broadcast(&c->changed);
   pthread_mutex_unlock(&c->lock);
   return result;
}

void
wsi_kolos_release_image(struct wsi_kolos_connection *c,
                        struct wsi_kolos_image *image)
{
   if (!c || !image)
      return;
   pthread_mutex_lock(&c->lock);
   struct wsi_kolos_pool *pool = find_pool(c, image->generation);
   if (pool && image->session_id == c->session.id &&
       image->slot < pool->count &&
       &pool->images[image->slot] == image &&
       pool->state[image->slot] == KOLOS_ACQUIRED) {
      pool->state[image->slot] = KOLOS_AVAILABLE;
      pool->leases--;
      collect_retired(c);
      pthread_cond_broadcast(&c->changed);
   }
   pthread_mutex_unlock(&c->lock);
}

VkResult
wsi_kolos_stop(struct wsi_kolos_connection *c)
{
   if (!c)
      return VK_ERROR_INITIALIZATION_FAILED;
   pthread_mutex_lock(&c->lock);
   if (c->done) {
      pthread_mutex_unlock(&c->lock);
      return VK_SUCCESS;
   }
   if (c->failure != VK_SUCCESS || c->closing) {
      VkResult result = c->failure != VK_SUCCESS ? c->failure : VK_ERROR_SURFACE_LOST_KHR;
      pthread_mutex_unlock(&c->lock);
      return result;
   }
   bool leased = c->session.current->leases != 0;
   for (struct wsi_kolos_pool *pool = c->session.retired; pool; pool = pool->next)
      leased |= pool->leases != 0;
   if (c->pending || leased) {
      pthread_mutex_unlock(&c->lock);
      return VK_ERROR_VALIDATION_FAILED_EXT;
   }
   if (c->stopping) {
      while (!c->done && c->failure == VK_SUCCESS && !c->closing)
         pthread_cond_wait(&c->changed, &c->lock);
      VkResult result = c->done ? VK_SUCCESS :
         (c->failure != VK_SUCCESS ? c->failure : VK_ERROR_SURFACE_LOST_KHR);
      pthread_mutex_unlock(&c->lock);
      return result;
   }
   c->stopping = true;
   uint8_t payload[16];
   put64(payload, c->session.id);
   put64(payload + 8, c->session.current->generation);
   bool sent = send_control(c, KGX_STOP, payload, sizeof(payload));
   if (!sent)
      c->failure = VK_ERROR_SURFACE_LOST_KHR;
   while (sent && !c->done && c->failure == VK_SUCCESS && !c->closing)
      pthread_cond_wait(&c->changed, &c->lock);
   VkResult result = c->done ? VK_SUCCESS :
      (c->failure != VK_SUCCESS ? c->failure : VK_ERROR_SURFACE_LOST_KHR);
   pthread_mutex_unlock(&c->lock);
   return result;
}

void
wsi_kolos_destroy(struct wsi_kolos_connection *c)
{
   if (!c)
      return;
   pthread_mutex_lock(&c->lock);
   c->closing = true;
   if (c->fd >= 0)
      shutdown(c->fd, SHUT_RDWR);
   if (c->import_fd >= 0)
      shutdown(c->import_fd, SHUT_RDWR);
   pthread_cond_broadcast(&c->changed);
   pthread_mutex_unlock(&c->lock);
   if (c->reader_started)
      pthread_join(c->reader, NULL);
   if (c->fd >= 0)
      close(c->fd);
   destroy_pool(c, c->session.current);
   while (c->session.retired) {
      struct wsi_kolos_pool *next = c->session.retired->next;
      destroy_pool(c, c->session.retired);
      c->session.retired = next;
   }
   pthread_cond_destroy(&c->changed);
   pthread_mutex_destroy(&c->write_lock);
   pthread_mutex_destroy(&c->lock);
   free(c->path);
   free(c);
}
