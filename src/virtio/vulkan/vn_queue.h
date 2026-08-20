/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#ifndef VN_QUEUE_H
#define VN_QUEUE_H

#include "vn_common.h"

struct vn_queue {
   struct vn_queue_base base;

   /* only used if renderer supports multiple timelines */
   uint32_t ring_idx;

   /* wait fence used for vn_QueueWaitIdle */
   VkFence wait_fence;

   /* Absolute Host display deadline returned by WineHua private present.
    * Vulkan queue operations are externally synchronized, so the next
    * present on this queue can safely consume and replace this value. */
   uint64_t winehua_next_present_deadline_ns;

   /* Ring seqno of the last async vkQueueSubmit on this queue. Present must
    * wait this seqno so the out-of-band vtest copy cannot run before the
    * writer submit is decoded on the Host. ROUNDTRIP_ONLY only skips
    * wait_all of unrelated ring commands; it must not skip this wait. */
   uint32_t winehua_last_submit_seqno;
   bool winehua_last_submit_seqno_valid;

   /* semaphore for gluing vkQueueSubmit feedback commands to
    * vkQueueBindSparse
    */
   VkSemaphore sparse_semaphore;
   uint64_t sparse_semaphore_counter;

   /* for vn_queue_submission storage */
   struct vn_cached_storage storage;
};
VK_DEFINE_HANDLE_CASTS(vn_queue, base.base.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

enum vn_sync_type {
   /* no payload */
   VN_SYNC_TYPE_INVALID,

   /* device object */
   VN_SYNC_TYPE_DEVICE_ONLY,

   /* payload is an imported sync file */
   VN_SYNC_TYPE_IMPORTED_SYNC_FD,
};

struct vn_sync_payload {
   enum vn_sync_type type;

   /* If type is VN_SYNC_TYPE_IMPORTED_SYNC_FD, fd is a sync file. */
   int fd;
};

/* For external fences and external semaphores submitted to be signaled. The
 * Vulkan spec guarantees those external syncs are on permanent payload.
 */
struct vn_sync_payload_external {
   /* ring_idx of the last queue submission */
   uint32_t ring_idx;
   /* true after a queue submission has installed this payload */
   bool submission_valid;
   /* valid when NO_ASYNC_QUEUE_SUBMIT perf option is not used */
   bool ring_seqno_valid;
   /* ring seqno of the last queue submission */
   uint32_t ring_seqno;
};

struct vn_feedback_slot;

struct vn_fence {
   struct vn_object_base base;

   struct vn_sync_payload *payload;

   struct vn_sync_payload permanent;
   struct vn_sync_payload temporary;

   struct {
      /* non-NULL if VN_PERF_NO_FENCE_FEEDBACK is disabled */
      struct vn_feedback_slot *slot;
      VkCommandBuffer *commands;
   } feedback;

   bool is_external;
   struct vn_sync_payload_external external_payload;

   /* Optional WineHua event-driven wait state.  The renderer sync is a
    * monotonically increasing timeline reused across fence reset/submission
    * cycles.  Host access to VkFence wait operations may be concurrent, so
    * protect lazy marker creation independently of Vulkan's reset/destroy
    * external-synchronization requirements. */
   simple_mtx_t winehua_event_mutex;
   struct vn_renderer_sync *winehua_event_sync;
   uint64_t winehua_event_value;
   bool winehua_event_submitted;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_fence,
                               base.base,
                               VkFence,
                               VK_OBJECT_TYPE_FENCE)

struct vn_semaphore {
   struct vn_object_base base;

   VkSemaphoreType type;

   struct vn_sync_payload *payload;

   struct vn_sync_payload permanent;
   struct vn_sync_payload temporary;

   struct {
      /* non-NULL if VN_PERF_NO_SEMAPHORE_FEEDBACK is disabled */
      struct vn_feedback_slot *slot;

      /* Lists of allocated vn_semaphore_feedback_cmd
       *
       * On submission prepare, sfb cmd is cache allocated from the free list
       * and is moved to the pending list after initialization.
       *
       * On submission cleanup, sfb cmds of the owner semaphores are checked
       * and cached to the free list if they have been "signaled", which is
       * proxyed via the src slot value having been reached.
       */
      struct list_head pending_cmds;
      struct list_head free_cmds;
      uint32_t free_cmd_count;

      /* Lock for accessing free/pending sfb cmds */
      simple_mtx_t cmd_mtx;

      /* Cached counter value to track if an async sem wait call is needed */
      uint64_t signaled_counter;

      /* Lock for checking if an async sem wait call is needed based on
       * the current counter value and signaled_counter to ensure async
       * wait order across threads.
       */
      simple_mtx_t async_wait_mtx;
   } feedback;

   bool is_external;
   struct vn_sync_payload_external external_payload;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_semaphore,
                               base.base,
                               VkSemaphore,
                               VK_OBJECT_TYPE_SEMAPHORE)

struct vn_event {
   struct vn_object_base base;

   /* non-NULL if below are satisfied:
    * - event is created without VK_EVENT_CREATE_DEVICE_ONLY_BIT
    * - VN_PERF_NO_EVENT_FEEDBACK is disabled
    */
   struct vn_feedback_slot *feedback_slot;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(vn_event,
                               base.base,
                               VkEvent,
                               VK_OBJECT_TYPE_EVENT)

void
vn_fence_signal_wsi(struct vn_device *dev, struct vn_fence *fence);

void
vn_semaphore_signal_wsi(struct vn_device *dev, struct vn_semaphore *sem);

#endif /* VN_QUEUE_H */
