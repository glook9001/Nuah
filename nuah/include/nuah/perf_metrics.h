#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime profiling is deliberately opt-in.  With NUAH_PERF_TRACE unset,
 * these functions return without touching a counter or taking a lock.  This
 * keeps the normal game path identical to a non-instrumented build.
 */
int nuah_perf_trace_enabled(void);
void nuah_perf_record_input(uint64_t duration_ns, unsigned int event_count);
void nuah_perf_record_jni(const char* kind, uint64_t duration_ns, int result);

#ifdef __cplusplus
}
#endif
