/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on virgl which is:
 * Copyright 2014, 2015 Red Hat.
 */

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "util/os_file.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/sparse_array.h"
#include "util/u_process.h"
#define VIRGL_RENDERER_UNSTABLE_APIS
#include "virtio-gpu/virglrenderer_hw.h"
#include "vtest/vtest_protocol.h"

#include "vn_renderer_internal.h"
#include "vn_device.h"
#include "vn_image.h"
#include "vn_queue.h"
#include "vn_ring.h"

#define VTEST_PCI_VENDOR_ID 0x1af4
#define VTEST_PCI_DEVICE_ID 0x1050

struct vtest;

static atomic_uint_fast64_t winehua_present_paced_waits;
static atomic_uint_fast64_t winehua_present_paced_wait_us;
static atomic_uint_fast64_t winehua_present_ring_drain_count;
static atomic_uint_fast64_t winehua_present_ring_drain_total_us;
static atomic_uint_fast64_t winehua_present_ring_drain_max_us;
static atomic_uint_fast64_t winehua_present_ring_drain_retries;

static void
vn_winehua_atomic_max(atomic_uint_fast64_t *value, uint64_t candidate)
{
   uint_fast64_t current = atomic_load_explicit(value, memory_order_relaxed);
   while (current < candidate &&
          !atomic_compare_exchange_weak_explicit(value, &current, candidate,
                                                 memory_order_relaxed,
                                                 memory_order_relaxed))
      ;
}

static bool
vn_winehua_present_image_trace_enabled(void)
{
   static atomic_int cached = ATOMIC_VAR_INIT(-1);
   int enabled = atomic_load_explicit(&cached, memory_order_relaxed);
   if (enabled < 0) {
      const char *value = os_get_option("WINEHUA_DXVK_TRACE_PRESENT_IMAGE");
      enabled = value && value[0] == '1' && !value[1];
      atomic_store_explicit(&cached, enabled, memory_order_relaxed);
   }
   return enabled != 0;
}

struct vtest_shmem {
   struct vn_renderer_shmem base;
};

struct vtest_bo {
   struct vn_renderer_bo base;

   uint32_t blob_flags;
   /* might be closed after mmap */
   int res_fd;
};

struct vtest_sync {
   struct vn_renderer_sync base;
};

struct vtest {
   struct vn_renderer base;

   struct vn_instance *instance;

   mtx_t sock_mutex;
   int sock_fd;

   uint32_t protocol_version;
   uint32_t max_timeline_count;

   struct {
      enum virgl_renderer_capset id;
      uint32_t version;
      struct virgl_renderer_capset_venus data;
   } capset;

   uint32_t shmem_blob_mem;

   struct util_sparse_array shmem_array;
   struct util_sparse_array bo_array;

   struct vn_renderer_shmem_cache shmem_cache;
};

static bool
vtest_winehua_present_trace_enabled(void)
{
   const char *value = os_get_option("VN_WINEHUA_PRESENT_TRACE");
   return value && value[0] == '1';
}

/* Diagnostic-only mode for the OHOS Host Vulkan shadow bridge.  It keeps the
 * Host resource alive after the Guest mmap is released so that the 3-second
 * shmem cache expiry cannot race a Venus ring/stream.  The resource is then
 * reclaimed with the vtest connection/context; this is intentionally not a
 * production lifetime policy. */
static bool
vtest_winehua_defer_shmem_unref_enabled(void)
{
   const char *value = os_get_option("VN_WINEHUA_DEFER_SHMEM_UNREF");
   return value && value[0] == '1';
}

static int
vtest_connect_socket(struct vn_instance *instance, const char *path)
{
   struct sockaddr_un un;
   int sock;

   sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (sock < 0) {
      vn_log(instance, "failed to create a socket");
      return -1;
   }

   memset(&un, 0, sizeof(un));
   un.sun_family = AF_UNIX;
   memcpy(un.sun_path, path, strlen(path));

   if (connect(sock, (struct sockaddr *)&un, sizeof(un)) == -1) {
      vn_log(instance, "failed to connect to %s: %s", path, strerror(errno));
      close(sock);
      return -1;
   }

   return sock;
}

static void
vtest_read(struct vtest *vtest, void *buf, size_t size)
{
   do {
      const ssize_t ret = read(vtest->sock_fd, buf, size);
      if (unlikely(ret < 0)) {
         vn_log(vtest->instance,
                "lost connection to rendering server on %zu read %zi %d",
                size, ret, errno);
         abort();
      }

      buf += ret;
      size -= ret;
   } while (size);
}

static int
vtest_receive_fd(struct vtest *vtest)
{
   char cmsg_buf[CMSG_SPACE(sizeof(int))];
   char dummy;
   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = &dummy,
            .iov_len = sizeof(dummy),
         },
      .msg_iovlen = 1,
      .msg_control = cmsg_buf,
      .msg_controllen = sizeof(cmsg_buf),
   };

   if (recvmsg(vtest->sock_fd, &msg, 0) < 0) {
      vn_log(vtest->instance, "recvmsg failed: %s", strerror(errno));
      abort();
   }

   struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
   if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
       cmsg->cmsg_type != SCM_RIGHTS) {
      vn_log(vtest->instance, "invalid cmsghdr");
      abort();
   }

   return *((int *)CMSG_DATA(cmsg));
}

static void
vtest_write(struct vtest *vtest, const void *buf, size_t size)
{
   do {
      const ssize_t ret = write(vtest->sock_fd, buf, size);
      if (unlikely(ret < 0)) {
         vn_log(vtest->instance,
                "lost connection to rendering server on %zu write %zi %d",
                size, ret, errno);
         abort();
      }

      buf += ret;
      size -= ret;
   } while (size);
}

static void
vtest_vcmd_create_renderer(struct vtest *vtest, const char *name)
{
   const size_t size = strlen(name) + 1;

   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = size;
   vtest_hdr[VTEST_CMD_ID] = VCMD_CREATE_RENDERER;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, name, size);
}

static bool
vtest_vcmd_ping_protocol_version(struct vtest *vtest)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_PING_PROTOCOL_VERSION_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_PING_PROTOCOL_VERSION;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));

   /* send a dummy busy wait to avoid blocking in vtest_read in case ping
    * protocol version is not supported
    */
   uint32_t vcmd_busy_wait[VCMD_BUSY_WAIT_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_BUSY_WAIT_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_BUSY_WAIT;
   vcmd_busy_wait[VCMD_BUSY_WAIT_HANDLE] = 0;
   vcmd_busy_wait[VCMD_BUSY_WAIT_FLAGS] = 0;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_busy_wait, sizeof(vcmd_busy_wait));

   uint32_t dummy;
   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   if (vtest_hdr[VTEST_CMD_ID] == VCMD_PING_PROTOCOL_VERSION) {
      /* consume the dummy busy wait result */
      vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
      assert(vtest_hdr[VTEST_CMD_ID] == VCMD_RESOURCE_BUSY_WAIT);
      vtest_read(vtest, &dummy, sizeof(dummy));
      return true;
   } else {
      /* no ping protocol version support */
      assert(vtest_hdr[VTEST_CMD_ID] == VCMD_RESOURCE_BUSY_WAIT);
      vtest_read(vtest, &dummy, sizeof(dummy));
      return false;
   }
}

static uint32_t
vtest_vcmd_protocol_version(struct vtest *vtest)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_protocol_version[VCMD_PROTOCOL_VERSION_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_PROTOCOL_VERSION_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_PROTOCOL_VERSION;
   vcmd_protocol_version[VCMD_PROTOCOL_VERSION_VERSION] =
      VTEST_PROTOCOL_VERSION;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_protocol_version, sizeof(vcmd_protocol_version));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == VCMD_PROTOCOL_VERSION_SIZE);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_PROTOCOL_VERSION);
   vtest_read(vtest, vcmd_protocol_version, sizeof(vcmd_protocol_version));

   return vcmd_protocol_version[VCMD_PROTOCOL_VERSION_VERSION];
}

static uint32_t
vtest_vcmd_get_param(struct vtest *vtest, enum vcmd_param param)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_get_param[VCMD_GET_PARAM_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_GET_PARAM_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_GET_PARAM;
   vcmd_get_param[VCMD_GET_PARAM_PARAM] = param;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_get_param, sizeof(vcmd_get_param));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 2);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_GET_PARAM);

   uint32_t resp[2];
   vtest_read(vtest, resp, sizeof(resp));

   return resp[0] ? resp[1] : 0;
}

static bool
vtest_vcmd_get_capset(struct vtest *vtest,
                      enum virgl_renderer_capset id,
                      uint32_t version,
                      void *capset,
                      size_t capset_size)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_get_capset[VCMD_GET_CAPSET_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_GET_CAPSET_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_GET_CAPSET;
   vcmd_get_capset[VCMD_GET_CAPSET_ID] = id;
   vcmd_get_capset[VCMD_GET_CAPSET_VERSION] = version;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_get_capset, sizeof(vcmd_get_capset));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_GET_CAPSET);

   uint32_t valid;
   vtest_read(vtest, &valid, sizeof(valid));
   if (!valid)
      return false;

   size_t read_size = (vtest_hdr[VTEST_CMD_LEN] - 1) * 4;
   if (capset_size >= read_size) {
      vtest_read(vtest, capset, read_size);
      memset(capset + read_size, 0, capset_size - read_size);
   } else {
      vtest_read(vtest, capset, capset_size);

      char temp[256];
      read_size -= capset_size;
      while (read_size) {
         const size_t temp_size = MIN2(read_size, ARRAY_SIZE(temp));
         vtest_read(vtest, temp, temp_size);
         read_size -= temp_size;
      }
   }

   return true;
}

static void
vtest_vcmd_context_init(struct vtest *vtest,
                        enum virgl_renderer_capset capset_id)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_context_init[VCMD_CONTEXT_INIT_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_CONTEXT_INIT_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_CONTEXT_INIT;
   vcmd_context_init[VCMD_CONTEXT_INIT_CAPSET_ID] = capset_id;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_context_init, sizeof(vcmd_context_init));
}

static uint32_t
vtest_vcmd_resource_create_blob(struct vtest *vtest,
                                enum vcmd_blob_type type,
                                uint32_t flags,
                                VkDeviceSize size,
                                vn_object_id blob_id,
                                int *res_fd)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_RES_CREATE_BLOB_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_CREATE_BLOB;

   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_TYPE] = type;
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_FLAGS] = flags;
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_SIZE_LO] = (uint32_t)size;
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_SIZE_HI] =
      (uint32_t)(size >> 32);
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_ID_LO] = (uint32_t)blob_id;
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_ID_HI] =
      (uint32_t)(blob_id >> 32);

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_res_create_blob, sizeof(vcmd_res_create_blob));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 1);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_RESOURCE_CREATE_BLOB);

   uint32_t res_id;
   vtest_read(vtest, &res_id, sizeof(res_id));

   *res_fd = vtest_receive_fd(vtest);

   return res_id;
}

static void
vtest_vcmd_resource_unref(struct vtest *vtest, uint32_t res_id)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_res_unref[VCMD_RES_UNREF_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_RES_UNREF_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_UNREF;
   vcmd_res_unref[VCMD_RES_UNREF_RES_HANDLE] = res_id;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_res_unref, sizeof(vcmd_res_unref));
}

static uint32_t
vtest_vcmd_sync_create(struct vtest *vtest, uint64_t initial_val)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_sync_create[VCMD_SYNC_CREATE_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_CREATE_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_CREATE;

   vcmd_sync_create[VCMD_SYNC_CREATE_VALUE_LO] = (uint32_t)initial_val;
   vcmd_sync_create[VCMD_SYNC_CREATE_VALUE_HI] =
      (uint32_t)(initial_val >> 32);

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_sync_create, sizeof(vcmd_sync_create));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 1);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_SYNC_CREATE);

   uint32_t sync_id;
   vtest_read(vtest, &sync_id, sizeof(sync_id));

   return sync_id;
}

static void
vtest_vcmd_sync_unref(struct vtest *vtest, uint32_t sync_id)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_sync_unref[VCMD_SYNC_UNREF_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_UNREF_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_UNREF;
   vcmd_sync_unref[VCMD_SYNC_UNREF_ID] = sync_id;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_sync_unref, sizeof(vcmd_sync_unref));
}

static uint64_t
vtest_vcmd_sync_read(struct vtest *vtest, uint32_t sync_id)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_sync_read[VCMD_SYNC_READ_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_READ_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_READ;

   vcmd_sync_read[VCMD_SYNC_READ_ID] = sync_id;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_sync_read, sizeof(vcmd_sync_read));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 2);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_SYNC_READ);

   uint64_t val;
   vtest_read(vtest, &val, sizeof(val));

   return val;
}

static void
vtest_vcmd_sync_write(struct vtest *vtest, uint32_t sync_id, uint64_t val)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_sync_write[VCMD_SYNC_WRITE_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_WRITE_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_WRITE;

   vcmd_sync_write[VCMD_SYNC_WRITE_ID] = sync_id;
   vcmd_sync_write[VCMD_SYNC_WRITE_VALUE_LO] = (uint32_t)val;
   vcmd_sync_write[VCMD_SYNC_WRITE_VALUE_HI] = (uint32_t)(val >> 32);

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_sync_write, sizeof(vcmd_sync_write));
}

static int
vtest_vcmd_sync_wait(struct vtest *vtest,
                     uint32_t flags,
                     int poll_timeout,
                     struct vn_renderer_sync *const *syncs,
                     const uint64_t *vals,
                     uint32_t count)
{
   const uint32_t timeout = poll_timeout >= 0 && poll_timeout <= INT32_MAX
                               ? poll_timeout
                               : UINT32_MAX;

   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_WAIT_SIZE(count);
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_WAIT;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, &flags, sizeof(flags));
   vtest_write(vtest, &timeout, sizeof(timeout));
   for (uint32_t i = 0; i < count; i++) {
      const uint64_t val = vals[i];
      const uint32_t sync[3] = {
         syncs[i]->sync_id,
         (uint32_t)val,
         (uint32_t)(val >> 32),
      };
      vtest_write(vtest, sync, sizeof(sync));
   }

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 0);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_SYNC_WAIT);

   return vtest_receive_fd(vtest);
}

static void
submit_cmd2_sizes(const struct vn_renderer_submit *submit,
                  size_t *header_size,
                  size_t *cs_size,
                  size_t *sync_size)
{
   if (!submit->batch_count) {
      *header_size = 0;
      *cs_size = 0;
      *sync_size = 0;
      return;
   }

   *header_size = sizeof(uint32_t) +
                  sizeof(struct vcmd_submit_cmd2_batch) * submit->batch_count;

   *cs_size = 0;
   *sync_size = 0;
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *batch = &submit->batches[i];
      assert(batch->cs_size % sizeof(uint32_t) == 0);
      *cs_size += batch->cs_size;
      *sync_size += (sizeof(uint32_t) + sizeof(uint64_t)) * batch->sync_count;
   }

   assert(*header_size % sizeof(uint32_t) == 0);
   assert(*cs_size % sizeof(uint32_t) == 0);
   assert(*sync_size % sizeof(uint32_t) == 0);
}

static void
vtest_vcmd_submit_cmd2(struct vtest *vtest,
                       const struct vn_renderer_submit *submit)
{
   size_t header_size;
   size_t cs_size;
   size_t sync_size;
   submit_cmd2_sizes(submit, &header_size, &cs_size, &sync_size);
   const size_t total_size = header_size + cs_size + sync_size;
   if (!total_size)
      return;

   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = total_size / sizeof(uint32_t);
   vtest_hdr[VTEST_CMD_ID] = VCMD_SUBMIT_CMD2;
   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));

   /* write batch count and batch headers */
   const uint32_t batch_count = submit->batch_count;
   size_t cs_offset = header_size;
   size_t sync_offset = cs_offset + cs_size;
   vtest_write(vtest, &batch_count, sizeof(batch_count));
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *batch = &submit->batches[i];
      struct vcmd_submit_cmd2_batch dst = {
         .flags = VCMD_SUBMIT_CMD2_FLAG_RING_IDX,
         .cmd_offset = cs_offset / sizeof(uint32_t),
         .cmd_size = batch->cs_size / sizeof(uint32_t),
         .sync_offset = sync_offset / sizeof(uint32_t),
         .sync_count = batch->sync_count,
         .ring_idx = batch->ring_idx,
      };
      vtest_write(vtest, &dst, sizeof(dst));

      cs_offset += batch->cs_size;
      sync_offset +=
         (sizeof(uint32_t) + sizeof(uint64_t)) * batch->sync_count;
   }

   /* write cs */
   if (cs_size) {
      for (uint32_t i = 0; i < submit->batch_count; i++) {
         const struct vn_renderer_submit_batch *batch = &submit->batches[i];
         if (batch->cs_size)
            vtest_write(vtest, batch->cs_data, batch->cs_size);
      }
   }

   /* write syncs */
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *batch = &submit->batches[i];

      for (uint32_t j = 0; j < batch->sync_count; j++) {
         const uint64_t val = batch->sync_values[j];
         const uint32_t sync[3] = {
            batch->syncs[j]->sync_id,
            (uint32_t)val,
            (uint32_t)(val >> 32),
         };
         vtest_write(vtest, sync, sizeof(sync));
      }
   }
}

static int
vtest_vcmd_winehua_vk_present(
   struct vtest *vtest,
   const struct vn_renderer_winehua_present *present)
{
   const uint32_t header[VTEST_HDR_SIZE] = {
      [VTEST_CMD_LEN] = VCMD_WINEHUA_VK_PRESENT_SIZE,
      [VTEST_CMD_ID] = VCMD_WINEHUA_VK_PRESENT,
   };
   const uint32_t command[VCMD_WINEHUA_VK_PRESENT_SIZE] = {
      [VCMD_WINEHUA_VK_PRESENT_PROTOCOL_VERSION] =
         VCMD_WINEHUA_VK_PRESENT_VERSION,
      [VCMD_WINEHUA_VK_PRESENT_FLAGS] = present->flags,
      [VCMD_WINEHUA_VK_PRESENT_QUEUE_ID_LO] = (uint32_t)present->queue_id,
      [VCMD_WINEHUA_VK_PRESENT_QUEUE_ID_HI] =
         (uint32_t)(present->queue_id >> 32),
      [VCMD_WINEHUA_VK_PRESENT_IMAGE_ID_LO] = (uint32_t)present->image_id,
      [VCMD_WINEHUA_VK_PRESENT_IMAGE_ID_HI] =
         (uint32_t)(present->image_id >> 32),
      [VCMD_WINEHUA_VK_PRESENT_WIDTH] = present->width,
      [VCMD_WINEHUA_VK_PRESENT_HEIGHT] = present->height,
      [VCMD_WINEHUA_VK_PRESENT_FORMAT] = present->format,
      [VCMD_WINEHUA_VK_PRESENT_LAYOUT] = present->layout,
      [VCMD_WINEHUA_VK_PRESENT_SERIAL] = present->serial,
      [VCMD_WINEHUA_VK_PRESENT_SURFACE_ID] = present->surface_id,
      [VCMD_WINEHUA_VK_PRESENT_CLIENT_PID] = present->client_pid,
   };
   uint32_t reply_header[VTEST_HDR_SIZE];
   uint32_t reply[VCMD_WINEHUA_VK_PRESENT_REPLY_SIZE];

   if (vtest_winehua_present_trace_enabled())
      vn_log(vtest->instance,
             "winehua vk present: write begin serial=%u queue=%" PRIu64
             " image=%" PRIu64,
             present->serial, present->queue_id, present->image_id);
   vtest_write(vtest, header, sizeof(header));
   vtest_write(vtest, command, sizeof(command));
   if (vtest_winehua_present_trace_enabled())
      vn_log(vtest->instance, "winehua vk present: waiting reply serial=%u",
             present->serial);
   vtest_read(vtest, reply_header, sizeof(reply_header));
   if (vtest_winehua_present_trace_enabled())
      vn_log(vtest->instance,
             "winehua vk present: reply header serial=%u len=%u id=%u",
             present->serial, reply_header[VTEST_CMD_LEN],
             reply_header[VTEST_CMD_ID]);
   if (reply_header[VTEST_CMD_ID] != VCMD_WINEHUA_VK_PRESENT ||
       reply_header[VTEST_CMD_LEN] != VCMD_WINEHUA_VK_PRESENT_REPLY_SIZE)
      return -EPROTO;

   vtest_read(vtest, reply, sizeof(reply));
   if (reply[VCMD_WINEHUA_VK_PRESENT_REPLY_SERIAL] != present->serial)
      return -EPROTO;

   if (present->next_present_deadline_ns) {
      *present->next_present_deadline_ns =
         (uint64_t)reply[VCMD_WINEHUA_VK_PRESENT_REPLY_DEADLINE_LO] |
         (uint64_t)reply[VCMD_WINEHUA_VK_PRESENT_REPLY_DEADLINE_HI] << 32;
   }

   if (vtest_winehua_present_trace_enabled())
      vn_log(vtest->instance, "winehua vk present: reply status=%d serial=%u",
             (int32_t)reply[VCMD_WINEHUA_VK_PRESENT_REPLY_STATUS],
             present->serial);
   return (int32_t)reply[VCMD_WINEHUA_VK_PRESENT_REPLY_STATUS];
}

static VkResult
vtest_sync_write(struct vn_renderer *renderer,
                 struct vn_renderer_sync *_sync,
                 uint64_t val)
{
   struct vtest *vtest = (struct vtest *)renderer;
   struct vtest_sync *sync = (struct vtest_sync *)_sync;

   mtx_lock(&vtest->sock_mutex);
   vtest_vcmd_sync_write(vtest, sync->base.sync_id, val);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static VkResult
vtest_sync_read(struct vn_renderer *renderer,
                struct vn_renderer_sync *_sync,
                uint64_t *val)
{
   struct vtest *vtest = (struct vtest *)renderer;
   struct vtest_sync *sync = (struct vtest_sync *)_sync;

   mtx_lock(&vtest->sock_mutex);
   *val = vtest_vcmd_sync_read(vtest, sync->base.sync_id);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static VkResult
vtest_sync_reset(struct vn_renderer *renderer,
                 struct vn_renderer_sync *sync,
                 uint64_t initial_val)
{
   /* same as write */
   return vtest_sync_write(renderer, sync, initial_val);
}

static void
vtest_sync_destroy(struct vn_renderer *renderer,
                   struct vn_renderer_sync *_sync)
{
   struct vtest *vtest = (struct vtest *)renderer;
   struct vtest_sync *sync = (struct vtest_sync *)_sync;

   mtx_lock(&vtest->sock_mutex);
   vtest_vcmd_sync_unref(vtest, sync->base.sync_id);
   mtx_unlock(&vtest->sock_mutex);

   free(sync);
}

static VkResult
vtest_sync_create(struct vn_renderer *renderer,
                  uint64_t initial_val,
                  uint32_t flags,
                  struct vn_renderer_sync **out_sync)
{
   struct vtest *vtest = (struct vtest *)renderer;

   struct vtest_sync *sync = calloc(1, sizeof(*sync));
   if (!sync)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   mtx_lock(&vtest->sock_mutex);
   sync->base.sync_id = vtest_vcmd_sync_create(vtest, initial_val);
   mtx_unlock(&vtest->sock_mutex);

   *out_sync = &sync->base;
   return VK_SUCCESS;
}

static void
vtest_bo_invalidate(struct vn_renderer *renderer,
                    struct vn_renderer_bo *bo,
                    VkDeviceSize offset,
                    VkDeviceSize size)
{
   /* nop */
}

static void
vtest_bo_flush(struct vn_renderer *renderer,
               struct vn_renderer_bo *bo,
               VkDeviceSize offset,
               VkDeviceSize size)
{
   /* nop */
}

static void *
vtest_bo_map(struct vn_renderer *renderer, struct vn_renderer_bo *_bo)
{
   struct vtest *vtest = (struct vtest *)renderer;
   struct vtest_bo *bo = (struct vtest_bo *)_bo;
   const bool mappable = bo->blob_flags & VCMD_BLOB_FLAG_MAPPABLE;
   const bool shareable = bo->blob_flags & VCMD_BLOB_FLAG_SHAREABLE;

   /* not thread-safe but is fine */
   if (!bo->base.mmap_ptr && mappable) {
      /* We wrongly assume that mmap(dma_buf) and vkMapMemory(VkDeviceMemory)
       * are equivalent when the blob type is VCMD_BLOB_TYPE_HOST3D.  While we
       * check for VCMD_PARAM_HOST_COHERENT_DMABUF_BLOB, we know vtest can
       * lie.
       */
      void *ptr = mmap(NULL, bo->base.mmap_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, bo->res_fd, 0);
      if (ptr == MAP_FAILED) {
         vn_log(vtest->instance, "failed to mmap %d of size %zu rw: %s",
                bo->res_fd, bo->base.mmap_size, strerror(errno));
      } else {
         bo->base.mmap_ptr = ptr;
         /* we don't need the fd anymore */
         if (!shareable) {
            close(bo->res_fd);
            bo->res_fd = -1;
         }
      }
   }

   return bo->base.mmap_ptr;
}

static int
vtest_bo_export_dma_buf(struct vn_renderer *renderer,
                        struct vn_renderer_bo *_bo)
{
   const struct vtest_bo *bo = (struct vtest_bo *)_bo;
   const bool shareable = bo->blob_flags & VCMD_BLOB_FLAG_SHAREABLE;
   return shareable ? os_dupfd_cloexec(bo->res_fd) : -1;
}

static bool
vtest_bo_destroy(struct vn_renderer *renderer, struct vn_renderer_bo *_bo)
{
   struct vtest *vtest = (struct vtest *)renderer;
   struct vtest_bo *bo = (struct vtest_bo *)_bo;

   if (bo->base.mmap_ptr)
      munmap(bo->base.mmap_ptr, bo->base.mmap_size);
   if (bo->res_fd >= 0)
      close(bo->res_fd);

   mtx_lock(&vtest->sock_mutex);
   vtest_vcmd_resource_unref(vtest, bo->base.res_id);
   mtx_unlock(&vtest->sock_mutex);

   return true;
}

static uint32_t
vtest_bo_blob_flags(VkMemoryPropertyFlags flags,
                    VkExternalMemoryHandleTypeFlags external_handles)
{
   uint32_t blob_flags = 0;
   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      blob_flags |= VCMD_BLOB_FLAG_MAPPABLE;
   if (external_handles)
      blob_flags |= VCMD_BLOB_FLAG_SHAREABLE;
   if (external_handles & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      blob_flags |= VCMD_BLOB_FLAG_CROSS_DEVICE;

   return blob_flags;
}

static VkResult
vtest_bo_create_from_device_memory(
   struct vn_renderer *renderer,
   VkDeviceSize size,
   vn_object_id mem_id,
   VkMemoryPropertyFlags flags,
   VkExternalMemoryHandleTypeFlags external_handles,
   struct vn_renderer_bo **out_bo)
{
   struct vtest *vtest = (struct vtest *)renderer;
   const uint32_t blob_flags = vtest_bo_blob_flags(flags, external_handles);

   mtx_lock(&vtest->sock_mutex);
   int res_fd;
   uint32_t res_id = vtest_vcmd_resource_create_blob(
      vtest, VCMD_BLOB_TYPE_HOST3D, blob_flags, size, mem_id, &res_fd);
   assert(res_id > 0 && res_fd >= 0);
   mtx_unlock(&vtest->sock_mutex);

   struct vtest_bo *bo = util_sparse_array_get(&vtest->bo_array, res_id);
   *bo = (struct vtest_bo){
      .base = {
         .refcount = VN_REFCOUNT_INIT(1),
         .res_id = res_id,
         .mmap_size = size,
      },
      .res_fd = res_fd,
      .blob_flags = blob_flags,
   };

   *out_bo = &bo->base;

   return VK_SUCCESS;
}

static void
vtest_shmem_destroy_now(struct vn_renderer *renderer,
                        struct vn_renderer_shmem *_shmem)
{
   struct vtest *vtest = (struct vtest *)renderer;
   struct vtest_shmem *shmem = (struct vtest_shmem *)_shmem;
   static atomic_uint_fast64_t deferred_unref_count;

   munmap(shmem->base.mmap_ptr, shmem->base.mmap_size);

   if (vtest_winehua_defer_shmem_unref_enabled()) {
      const uint64_t count = atomic_fetch_add_explicit(
         &deferred_unref_count, 1, memory_order_relaxed) + 1;
      if (count <= 8 || !(count % 120))
         vn_log(vtest->instance,
                "WineHua shmem unref deferred res=%u size=%zu count=%" PRIu64,
                shmem->base.res_id, shmem->base.mmap_size, count);
      return;
   }

   mtx_lock(&vtest->sock_mutex);
   vtest_vcmd_resource_unref(vtest, shmem->base.res_id);
   mtx_unlock(&vtest->sock_mutex);
}

static void
vtest_shmem_destroy(struct vn_renderer *renderer,
                    struct vn_renderer_shmem *shmem)
{
   struct vtest *vtest = (struct vtest *)renderer;

   if (vn_renderer_shmem_cache_add(&vtest->shmem_cache, shmem))
      return;

   vtest_shmem_destroy_now(&vtest->base, shmem);
}

static struct vn_renderer_shmem *
vtest_shmem_create(struct vn_renderer *renderer, size_t size)
{
   struct vtest *vtest = (struct vtest *)renderer;

   struct vn_renderer_shmem *cached_shmem =
      vn_renderer_shmem_cache_get(&vtest->shmem_cache, size);
   if (cached_shmem) {
      cached_shmem->refcount = VN_REFCOUNT_INIT(1);
      return cached_shmem;
   }

   mtx_lock(&vtest->sock_mutex);
   int res_fd;
   uint32_t res_id = vtest_vcmd_resource_create_blob(
      vtest, vtest->shmem_blob_mem, VCMD_BLOB_FLAG_MAPPABLE, size, 0,
      &res_fd);
   assert(res_id > 0 && res_fd >= 0);
   mtx_unlock(&vtest->sock_mutex);

   void *ptr =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, res_fd, 0);
   close(res_fd);
   if (ptr == MAP_FAILED) {
      mtx_lock(&vtest->sock_mutex);
      vtest_vcmd_resource_unref(vtest, res_id);
      mtx_unlock(&vtest->sock_mutex);
      return NULL;
   }

   struct vtest_shmem *shmem =
      util_sparse_array_get(&vtest->shmem_array, res_id);
   *shmem = (struct vtest_shmem){
      .base = {
         .refcount = VN_REFCOUNT_INIT(1),
         .res_id = res_id,
         .mmap_size = size,
         .mmap_ptr = ptr,
      },
   };

   return &shmem->base;
}

static VkResult
sync_wait_poll(int fd, int poll_timeout)
{
   struct pollfd pollfd = {
      .fd = fd,
      .events = POLLIN,
   };
   int ret;
   do {
      ret = poll(&pollfd, 1, poll_timeout);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   if (ret < 0 || (ret > 0 && !(pollfd.revents & POLLIN))) {
      return (ret < 0 && errno == ENOMEM) ? VK_ERROR_OUT_OF_HOST_MEMORY
                                          : VK_ERROR_DEVICE_LOST;
   }

   return ret ? VK_SUCCESS : VK_TIMEOUT;
}

static int
timeout_to_poll_timeout(uint64_t timeout)
{
   const uint64_t ns_per_ms = 1000000;
   const uint64_t ms = (timeout + ns_per_ms - 1) / ns_per_ms;
   if (!ms && timeout)
      return -1;
   return ms <= INT_MAX ? ms : -1;
}

static VkResult
vtest_wait(struct vn_renderer *renderer, const struct vn_renderer_wait *wait)
{
   struct vtest *vtest = (struct vtest *)renderer;
   const uint32_t flags = wait->wait_any ? VCMD_SYNC_WAIT_FLAG_ANY : 0;
   const int poll_timeout = timeout_to_poll_timeout(wait->timeout);

   /*
    * vtest_vcmd_sync_wait (and some other sync commands) is executed after
    * all prior commands are dispatched.  That is far from ideal.
    *
    * In virtio-gpu, a drm_syncobj wait ioctl is executed immediately.  It
    * works because it uses virtio-gpu interrupts as a side channel.  vtest
    * needs a side channel to perform well.
    *
    * virtio-gpu or vtest, we should also set up a 1-byte coherent memory that
    * is set to non-zero by GPU after the syncs signal.  That would allow us
    * to do a quick check (or spin a bit) before waiting.
    */
   mtx_lock(&vtest->sock_mutex);
   const int fd =
      vtest_vcmd_sync_wait(vtest, flags, poll_timeout, wait->syncs,
                           wait->sync_values, wait->sync_count);
   mtx_unlock(&vtest->sock_mutex);

   VkResult result = sync_wait_poll(fd, poll_timeout);
   close(fd);

   return result;
}

static VkResult
vtest_submit(struct vn_renderer *renderer,
             const struct vn_renderer_submit *submit)
{
   struct vtest *vtest = (struct vtest *)renderer;

   mtx_lock(&vtest->sock_mutex);
   vtest_vcmd_submit_cmd2(vtest, submit);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static int
vtest_winehua_present(
   struct vn_renderer *renderer,
   const struct vn_renderer_winehua_present *present)
{
   struct vtest *vtest = (struct vtest *)renderer;

   if (vtest_winehua_present_trace_enabled())
      vn_log(vtest->instance, "winehua vk present: lock socket serial=%u",
             present->serial);
   mtx_lock(&vtest->sock_mutex);
   const int result = vtest_vcmd_winehua_vk_present(vtest, present);
   mtx_unlock(&vtest->sock_mutex);
   if (vtest_winehua_present_trace_enabled())
      vn_log(vtest->instance,
             "winehua vk present: unlock socket serial=%u result=%d",
             present->serial, result);

   return result;
}

static void
vtest_init_renderer_info(struct vtest *vtest)
{
   struct vn_renderer_info *info = &vtest->base.info;

   info->pci.vendor_id = VTEST_PCI_VENDOR_ID;
   info->pci.device_id = VTEST_PCI_DEVICE_ID;

   info->has_dma_buf_import = false;
   info->has_external_sync = false;
   info->has_implicit_fencing = false;

   const struct virgl_renderer_capset_venus *capset = &vtest->capset.data;
   info->wire_format_version = capset->wire_format_version;
   info->vk_xml_version = capset->vk_xml_version;
   info->vk_ext_command_serialization_spec_version =
      capset->vk_ext_command_serialization_spec_version;
   info->vk_mesa_venus_protocol_spec_version =
      capset->vk_mesa_venus_protocol_spec_version;
   assert(capset->supports_blob_id_0);

   /* ensure vk_extension_mask is large enough to hold all capset masks */
   STATIC_ASSERT(sizeof(info->vk_extension_mask) >=
                 sizeof(capset->vk_extension_mask1));
   memcpy(info->vk_extension_mask, capset->vk_extension_mask1,
          sizeof(capset->vk_extension_mask1));

   assert(capset->allow_vk_wait_syncs);

   assert(capset->supports_multiple_timelines);
   info->max_timeline_count = vtest->max_timeline_count;
}

static void
vtest_destroy(struct vn_renderer *renderer,
              const VkAllocationCallbacks *alloc)
{
   struct vtest *vtest = (struct vtest *)renderer;

   vn_renderer_shmem_cache_fini(&vtest->shmem_cache);

   if (vtest->sock_fd >= 0) {
      shutdown(vtest->sock_fd, SHUT_RDWR);
      close(vtest->sock_fd);
   }

   mtx_destroy(&vtest->sock_mutex);
   util_sparse_array_finish(&vtest->shmem_array);
   util_sparse_array_finish(&vtest->bo_array);

   vk_free(alloc, vtest);
}

static VkResult
vtest_init_capset(struct vtest *vtest)
{
   vtest->capset.id = VIRGL_RENDERER_CAPSET_VENUS;
   vtest->capset.version = 0;

   if (!vtest_vcmd_get_capset(vtest, vtest->capset.id, vtest->capset.version,
                              &vtest->capset.data,
                              sizeof(vtest->capset.data))) {
      vn_log(vtest->instance, "no venus capset");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

static VkResult
vtest_init_params(struct vtest *vtest)
{
   uint32_t val = vtest_vcmd_get_param(vtest, VCMD_PARAM_MAX_TIMELINE_COUNT);
   if (!val) {
      vn_log(vtest->instance, "no timeline support");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   vtest->max_timeline_count = val;

   return VK_SUCCESS;
}

static VkResult
vtest_init_protocol_version(struct vtest *vtest)
{
   const uint32_t min_protocol_version = 3;

   const uint32_t ver = vtest_vcmd_ping_protocol_version(vtest)
                           ? vtest_vcmd_protocol_version(vtest)
                           : 0;
   if (ver < min_protocol_version) {
      vn_log(vtest->instance, "vtest protocol version (%d) too old", ver);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   vtest->protocol_version = ver;

   return VK_SUCCESS;
}

static VkResult
vtest_init(struct vtest *vtest)
{
   const char *socket_name = os_get_option("VTEST_SOCKET_NAME");

   util_sparse_array_init(&vtest->shmem_array, sizeof(struct vtest_shmem),
                          1024);
   util_sparse_array_init(&vtest->bo_array, sizeof(struct vtest_bo), 1024);

   mtx_init(&vtest->sock_mutex, mtx_plain);
   vtest->sock_fd = vtest_connect_socket(
      vtest->instance, socket_name ? socket_name : VTEST_DEFAULT_SOCKET_NAME);
   if (vtest->sock_fd < 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   const char *renderer_name = util_get_process_name();
   if (!renderer_name)
      renderer_name = "venus";
   vtest_vcmd_create_renderer(vtest, renderer_name);

   VkResult result = vtest_init_protocol_version(vtest);
   if (result == VK_SUCCESS)
      result = vtest_init_params(vtest);
   if (result == VK_SUCCESS)
      result = vtest_init_capset(vtest);
   if (result != VK_SUCCESS)
      return result;

   /* see virtgpu_init_shmem_blob_mem */
   assert(vtest->capset.data.supports_blob_id_0);
   vtest->shmem_blob_mem = VCMD_BLOB_TYPE_HOST3D;

   vn_renderer_shmem_cache_init(&vtest->shmem_cache, &vtest->base,
                                vtest_shmem_destroy_now);

   vtest_vcmd_context_init(vtest, vtest->capset.id);

   vtest_init_renderer_info(vtest);

   vtest->base.ops.destroy = vtest_destroy;
   vtest->base.ops.submit = vtest_submit;
   vtest->base.ops.wait = vtest_wait;
   vtest->base.ops.winehua_present = vtest_winehua_present;

   vtest->base.shmem_ops.create = vtest_shmem_create;
   vtest->base.shmem_ops.destroy = vtest_shmem_destroy;

   vtest->base.bo_ops.create_from_device_memory =
      vtest_bo_create_from_device_memory;
   vtest->base.bo_ops.create_from_dma_buf = NULL;
   vtest->base.bo_ops.destroy = vtest_bo_destroy;
   vtest->base.bo_ops.export_dma_buf = vtest_bo_export_dma_buf;
   vtest->base.bo_ops.map = vtest_bo_map;
   vtest->base.bo_ops.flush = vtest_bo_flush;
   vtest->base.bo_ops.invalidate = vtest_bo_invalidate;

   vtest->base.sync_ops.create = vtest_sync_create;
   vtest->base.sync_ops.create_from_syncobj = NULL;
   vtest->base.sync_ops.destroy = vtest_sync_destroy;
   vtest->base.sync_ops.export_syncobj = NULL;
   vtest->base.sync_ops.reset = vtest_sync_reset;
   vtest->base.sync_ops.read = vtest_sync_read;
   vtest->base.sync_ops.write = vtest_sync_write;

   return VK_SUCCESS;
}

VkResult
vn_renderer_create_vtest(struct vn_instance *instance,
                         const VkAllocationCallbacks *alloc,
                         struct vn_renderer **renderer)
{
   struct vtest *vtest = vk_zalloc(alloc, sizeof(*vtest), VN_DEFAULT_ALIGN,
                                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!vtest)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   vtest->instance = instance;
   vtest->sock_fd = -1;

   VkResult result = vtest_init(vtest);
   if (result != VK_SUCCESS) {
      vtest_destroy(&vtest->base, alloc);
      return result;
   }

   *renderer = &vtest->base;

   return VK_SUCCESS;
}

__attribute__((visibility("default"))) int
vn_winehua_present(VkQueue queue_handle,
                   VkImage image_handle,
                   uint32_t width,
                   uint32_t height,
                   VkFormat format,
                   VkImageLayout layout,
                   uint32_t client_pid,
                   uint32_t surface_id,
                   uint32_t serial,
                   uint64_t *next_present_deadline_ns)
{
   struct vn_queue *queue = vn_queue_from_handle(queue_handle);
   struct vn_image *image = vn_image_from_handle(image_handle);
   if (!queue || !image || !width || !height || !client_pid || !surface_id)
      return -EINVAL;

   struct vn_device *dev = (void *)queue->base.base.base.device;
   if (vn_winehua_present_image_trace_enabled())
      vn_log(dev->instance,
             "WineHuaPresentImage: layer=guest event=present serial=%u "
             "rawImage=0x%" PRIx64 " imageId=%" PRIu64 " queueId=%" PRIu64,
             serial, (uint64_t)(uintptr_t)image_handle, image->base.id,
             queue->base.id);
   const uint64_t pacing_deadline_ns =
      queue->winehua_next_present_deadline_ns;
   queue->winehua_next_present_deadline_ns = 0;

   /* Wait for the previous Host-provided deadline only after the application
    * has rendered its next frame and entered present.  This overlaps game
    * rendering with the display period instead of serializing render time
    * after an immediate post-present sleep. */
   const int64_t pacing_now_ns = os_time_get_nano();
   if (pacing_now_ns > 0 && pacing_deadline_ns > (uint64_t)pacing_now_ns) {
      const uint64_t remaining_ns =
         pacing_deadline_ns - (uint64_t)pacing_now_ns;
      if (remaining_ns <= 100000000ull) {
         const int64_t wait_start_ns = os_time_get_nano();
         os_time_sleep((int64_t)((remaining_ns + 999ull) / 1000ull));
         const int64_t wait_end_ns = os_time_get_nano();
         const uint64_t waited_us = wait_end_ns > wait_start_ns
            ? (uint64_t)(wait_end_ns - wait_start_ns) / 1000ull : 0;
         const uint64_t wait_count = atomic_fetch_add_explicit(
            &winehua_present_paced_waits, 1, memory_order_relaxed) + 1;
         const uint64_t total_us = atomic_fetch_add_explicit(
            &winehua_present_paced_wait_us, waited_us,
            memory_order_relaxed) + waited_us;
         if (wait_count <= 8 || !(wait_count % 120))
            vn_log(dev->instance,
                   "winehua vk present: paced_waits=%" PRIu64
                   " last_us=%" PRIu64 " total_us=%" PRIu64,
                   wait_count, waited_us, total_us);
      }
   }

   const struct vn_renderer_winehua_present present = {
      .queue_id = queue->base.id,
      .image_id = image->base.id,
      .width = width,
      .height = height,
      .format = format,
      .layout = layout,
      .client_pid = client_pid,
      .surface_id = surface_id,
      .serial = serial,
      .next_present_deadline_ns = next_present_deadline_ns,
   };

   /* The private present request uses the vtest socket while Vulkan object
    * creation and queue submission travel through the Venus ring.  A Venus
    * roundtrip only inserts a cross-transport marker; it does not wait for the
    * ring worker to consume commands before returning.  Drain through that
    * marker before the out-of-band lookup so present cannot acquire the Host
    * queue mutex ahead of the producer QueueSubmit.  This waits for renderer
    * decode and the Host driver's QueueSubmit call, not GPU completion.
    *
    * Treat -EAGAIN as a bounded object-publication race and retry here.  Never
    * expose that transient errno to Wine: the Vulkan thunk maps a negative
    * result to DEVICE_LOST, poisoning an otherwise valid x86 process. */
   const bool drain_perf =
      vn_ring_perf_summary_enabled(dev->primary_ring);
   uint64_t present_drain_us = 0;
   unsigned present_drain_attempts = 0;
   int result = -EAGAIN;
   for (unsigned attempt = 0; attempt < 8 && result == -EAGAIN; attempt++) {
      present_drain_attempts = attempt + 1;
      const int64_t drain_start_ns =
         (drain_perf || vtest_winehua_present_trace_enabled())
            ? os_time_get_nano() : 0;
      vn_ring_roundtrip(dev->primary_ring);
      vn_ring_wait_all(dev->primary_ring);
      if (drain_start_ns) {
         const int64_t drain_end_ns = os_time_get_nano();
         const uint64_t drain_us = drain_end_ns > drain_start_ns
            ? (uint64_t)(drain_end_ns - drain_start_ns) / 1000ull : 0;
         present_drain_us += drain_us;
         if (vtest_winehua_present_trace_enabled())
            vn_log(dev->instance,
                   "winehua vk present: ring drained serial=%u attempt=%u wait_us=%" PRIu64,
                   serial, attempt + 1, drain_us);
      }
      result = vn_renderer_winehua_present(dev->renderer, &present);
      if (result == -EAGAIN) {
         vn_log(dev->instance,
                "winehua vk present: object publication pending serial=%u attempt=%u",
                serial, attempt + 1);
         usleep(1000u << attempt);
      }
   }

   if (drain_perf) {
      const uint64_t count = atomic_fetch_add_explicit(
         &winehua_present_ring_drain_count, 1, memory_order_relaxed) + 1;
      const uint64_t total_us = atomic_fetch_add_explicit(
         &winehua_present_ring_drain_total_us, present_drain_us,
         memory_order_relaxed) + present_drain_us;
      vn_winehua_atomic_max(&winehua_present_ring_drain_max_us,
                            present_drain_us);
      const uint64_t retries = present_drain_attempts > 1
         ? atomic_fetch_add_explicit(
              &winehua_present_ring_drain_retries,
              present_drain_attempts - 1, memory_order_relaxed) +
              present_drain_attempts - 1
         : atomic_load_explicit(&winehua_present_ring_drain_retries,
                                memory_order_relaxed);
      if (count <= 8 || !(count % 120))
         vn_log(dev->instance,
                "winehua vk present: ring drain summary presents=%" PRIu64
                " total_us=%" PRIu64 " avg_us=%" PRIu64
                " max_us=%" PRIuFAST64 " retries=%" PRIu64,
                count, total_us, total_us / count,
                atomic_load_explicit(&winehua_present_ring_drain_max_us,
                                     memory_order_relaxed),
                retries);
   }

   if ((result == 0 || result == 1) && next_present_deadline_ns)
      queue->winehua_next_present_deadline_ns = *next_present_deadline_ns;

   return result;
}
