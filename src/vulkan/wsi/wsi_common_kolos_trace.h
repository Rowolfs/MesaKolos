#ifndef WSI_COMMON_KOLOS_TRACE_H
#define WSI_COMMON_KOLOS_TRACE_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* This timestamp is comparable with the Android and compositor traces. */
static inline void
wsi_kolos_trace(const char *event, uint64_t session, uint64_t generation,
                uint32_t slot, int64_t serial, int fence_fd)
{
   struct timespec now = {0};
   clock_gettime(CLOCK_MONOTONIC, &now);
   uint64_t ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
   fprintf(stderr,
           "KOLOS_SYNC t_ns=%" PRIu64 " side=mesa event=%s session=%" PRIu64
           " generation=%" PRIu64 " slot=%u serial=%" PRId64 " fence_fd=%d\n",
           ns, event, session, generation, slot, serial, fence_fd);
}

#endif
