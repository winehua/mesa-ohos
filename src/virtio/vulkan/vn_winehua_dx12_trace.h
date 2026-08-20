#ifndef VN_WINEHUA_DX12_TRACE_H
#define VN_WINEHUA_DX12_TRACE_H

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/os_misc.h"
#include "util/os_time.h"

/* Guest-side DX12 smoke frame accounting. Enabled by
 * VN_WINEHUA_DX12_FRAME_TRACE=1. Writes [DX12-FRAME] lines to stderr and
 * $WINEPREFIX/drive_c/smoke/results/dx12-frame.log. No behavior change.
 *
 * Counters live in one shared object (defined in vn_queue.c). A static
 * inline in this header would give each translation unit its own copy,
 * which previously dropped Present and Map-flush time from the fence-wait
 * emit path. */

struct vn_winehua_dx12_tls {
   atomic_int_fast64_t frame_start_ns;
   atomic_uint_fast64_t map_flush_us;
   atomic_uint_fast64_t map_scan_bytes;
   atomic_uint_fast64_t map_ranges;
   atomic_uint_fast64_t submit_us;
   atomic_uint_fast64_t submit_count;
   atomic_uint_fast64_t present_roundtrip_us;
   atomic_uint_fast64_t present_wait_seqno_us;
   atomic_uint_fast64_t present_renderer_us;
   atomic_uint_fast64_t present_total_us;
   atomic_uint_fast32_t writer_seqno;
   atomic_int writer_seqno_valid;
   atomic_int wait_all_called;
   atomic_uint_fast64_t fence_wait_us;
   atomic_uint_fast64_t frames;
};

extern struct vn_winehua_dx12_tls vn_winehua_dx12_acc;

static inline bool
vn_winehua_dx12_trace_enabled(void)
{
   static atomic_int cached = ATOMIC_VAR_INIT(-1);
   int enabled = atomic_load_explicit(&cached, memory_order_relaxed);
   if (enabled < 0) {
      const char *value = os_get_option("VN_WINEHUA_DX12_FRAME_TRACE");
      enabled = value && value[0] == '1' && !value[1];
      atomic_store_explicit(&cached, enabled, memory_order_relaxed);
   }
   return enabled != 0;
}

static inline struct vn_winehua_dx12_tls *
vn_winehua_dx12_tls(void)
{
   return &vn_winehua_dx12_acc;
}

static inline void
vn_winehua_dx12_add(atomic_uint_fast64_t *field, uint64_t value)
{
   if (value)
      atomic_fetch_add_explicit(field, value, memory_order_relaxed);
}

static inline uint64_t
vn_winehua_dx12_ns_to_us(int64_t start_ns, int64_t end_ns)
{
   if (!start_ns || end_ns <= start_ns)
      return 0;
   return (uint64_t)(end_ns - start_ns) / 1000ull;
}

static inline void
vn_winehua_dx12_note_start(void)
{
   if (!vn_winehua_dx12_trace_enabled())
      return;
   int64_t expected = 0;
   const int64_t now = os_time_get_nano();
   atomic_compare_exchange_strong_explicit(&vn_winehua_dx12_acc.frame_start_ns,
                                           &expected, now,
                                           memory_order_relaxed,
                                           memory_order_relaxed);
}

static inline void
vn_winehua_dx12_emit_and_reset(void)
{
   if (!vn_winehua_dx12_trace_enabled())
      return;

   struct vn_winehua_dx12_tls *t = &vn_winehua_dx12_acc;
   const int64_t now_ns = os_time_get_nano();
   const int64_t start_ns =
      atomic_load_explicit(&t->frame_start_ns, memory_order_relaxed);
   const uint64_t total_us = vn_winehua_dx12_ns_to_us(start_ns, now_ns);
   const uint64_t map_flush_us =
      atomic_exchange_explicit(&t->map_flush_us, 0, memory_order_relaxed);
   const uint64_t map_scan_bytes =
      atomic_exchange_explicit(&t->map_scan_bytes, 0, memory_order_relaxed);
   const uint64_t map_ranges =
      atomic_exchange_explicit(&t->map_ranges, 0, memory_order_relaxed);
   const uint64_t submit_us =
      atomic_exchange_explicit(&t->submit_us, 0, memory_order_relaxed);
   const uint64_t submit_count =
      atomic_exchange_explicit(&t->submit_count, 0, memory_order_relaxed);
   const uint64_t present_roundtrip_us =
      atomic_exchange_explicit(&t->present_roundtrip_us, 0,
                               memory_order_relaxed);
   const uint64_t present_wait_seqno_us =
      atomic_exchange_explicit(&t->present_wait_seqno_us, 0,
                               memory_order_relaxed);
   const uint64_t present_renderer_us =
      atomic_exchange_explicit(&t->present_renderer_us, 0,
                               memory_order_relaxed);
   const uint64_t present_total_us =
      atomic_exchange_explicit(&t->present_total_us, 0, memory_order_relaxed);
   const uint32_t writer_seqno =
      atomic_exchange_explicit(&t->writer_seqno, 0, memory_order_relaxed);
   const int writer_seqno_valid =
      atomic_exchange_explicit(&t->writer_seqno_valid, 0, memory_order_relaxed);
   const int wait_all_called =
      atomic_exchange_explicit(&t->wait_all_called, 0, memory_order_relaxed);
   const uint64_t fence_wait_us =
      atomic_exchange_explicit(&t->fence_wait_us, 0, memory_order_relaxed);
   const uint64_t frames =
      atomic_fetch_add_explicit(&t->frames, 1, memory_order_relaxed) + 1;
   atomic_store_explicit(&t->frame_start_ns, now_ns, memory_order_relaxed);

   const uint64_t accounted_us =
      map_flush_us + submit_us + present_total_us + fence_wait_us;
   const uint64_t unaccounted_us =
      total_us > accounted_us ? total_us - accounted_us : 0;

   const char *klass = "OK";
   if (map_flush_us >= 20000)
      klass = "MAP_SYNC_STALL";
   else if (present_wait_seqno_us >= 20000)
      klass = "PRESENT_WRITER_WAIT_STALL";
   else if (wait_all_called)
      klass = "PRESENT_WRITER_WAIT_STALL";
   else if (present_total_us >= 20000)
      klass = "PRESENT_WRITER_WAIT_STALL";
   else if (fence_wait_us >= 20000)
      klass = "GPU_FENCE_STALL";
   else if (unaccounted_us >= 20000)
      klass = "UNACCOUNTED_STALL";

   char line[896];
   snprintf(line, sizeof(line),
            "[DX12-FRAME] frame=%" PRIu64
            " map_sync_us=%" PRIu64 " map_scan_bytes=%" PRIu64
            " map_ranges=%" PRIu64
            " submit_us=%" PRIu64 " submit_count=%" PRIu64
            " present_roundtrip_us=%" PRIu64
            " present_writer_wait_us=%" PRIu64
            " present_renderer_us=%" PRIu64
            " present_total_us=%" PRIu64
            " writer_seqno=%u writer_seqno_valid=%d wait_all=%d"
            " fence_wait_us=%" PRIu64
            " total_us=%" PRIu64 " accounted_us=%" PRIu64
            " unaccounted_us=%" PRIu64 " CLASS=%s",
            frames, map_flush_us, map_scan_bytes, map_ranges,
            submit_us, submit_count, present_roundtrip_us,
            present_wait_seqno_us, present_renderer_us, present_total_us,
            writer_seqno, writer_seqno_valid, wait_all_called,
            fence_wait_us, total_us, accounted_us, unaccounted_us, klass);

   const bool sample = frames <= 16 || (frames % 30) == 0 ||
      strcmp(klass, "OK") != 0;
   if (sample)
      fprintf(stderr, "%s\n", line);
   const char *prefix = getenv("WINEPREFIX");
   if (prefix && prefix[0]) {
      char path[512];
      snprintf(path, sizeof(path),
               "%s/drive_c/smoke/results/dx12-frame.log", prefix);
      FILE *fp = fopen(path, "a");
      if (fp) {
         fputs(line, fp);
         fputc('\n', fp);
         fclose(fp);
      }
   }
}

#endif /* VN_WINEHUA_DX12_TRACE_H */
