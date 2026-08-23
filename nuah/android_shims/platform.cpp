#include "nuah/native_window_bridge.h"
#include "nuah/android_abi_registry.h"

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <mutex>
#include <poll.h>
#include <vector>
#include <unistd.h>

#ifndef NUAH_LIKELY
#define NUAH_LIKELY(x) (__builtin_expect(!!(x), 1))
#endif
#ifndef NUAH_UNLIKELY
#define NUAH_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif

namespace {
bool looper_trace_enabled() {
  /* ALooper_pollOnce is a hot path when Roblox asks for a non-blocking poll.
   * The environment never changes during a process, so do not call getenv on
   * every poll (or every ANativeWindow query) just to decide whether to log. */
  static const bool enabled = [] {
    const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
    return trace && *trace && std::strcmp(trace, "0") != 0;
  }();
  return enabled;
}

void unsupported_media(const char* symbol) {
  nuah_android_api_unsupported("libmediandk.so", symbol);
  errno = ENOSYS;
}
void unsupported_audio(const char* symbol) {
  nuah_android_api_unsupported("libOpenSLES.so", symbol);
  errno = ENOSYS;
}

bool zstd_trace_enabled() {
  static const bool enabled = [] {
    const char* trace = std::getenv("NUAH_ZSTD_TRACE");
    return trace && *trace && std::strcmp(trace, "0") != 0;
  }();
  return enabled;
}

bool zstd_trace_stack_enabled() {
  static const bool enabled = [] {
    const char* trace = std::getenv("NUAH_ZSTD_TRACE_STACK");
    return trace && *trace && std::strcmp(trace, "0") != 0;
  }();
  return enabled;
}

unsigned long long zstd_trace_slow_threshold_ns() {
  static const unsigned long long threshold = [] {
    const char* raw = std::getenv("NUAH_ZSTD_TRACE_SLOW_US");
    if (!raw || !*raw) return 2000ULL;
    char* end = nullptr;
    const auto parsed = std::strtoull(raw, &end, 10);
    return end != raw && *end == '\0' && parsed > 0 ? parsed : 2000ULL;
  }();
  return threshold * 1000ULL;
}

using SteadyClock = std::chrono::steady_clock;

std::atomic<unsigned long long> zstd_decompress_calls{0};
std::atomic<unsigned long long> zstd_decompress_ns{0};
std::atomic<unsigned long long> zstd_decompress_max_ns{0};
std::atomic<unsigned long long> zstd_compressed_bytes{0};
std::atomic<unsigned long long> zstd_uncompressed_bytes{0};
std::atomic<long long> zstd_next_report_ns{0};

/* Zstd's trace interface is ABI-stable through `version`. Roblox statically
 * links a trace-enabled Zstd and resolves these weak imports from Nuah's
 * Android provider, so preserve the layout instead of using ellipsis hooks. */
struct ZstdTrace {
  unsigned version;
  int streaming;
  unsigned dictionary_id;
  int dictionary_is_cold;
  std::size_t dictionary_size;
  std::size_t uncompressed_size;
  std::size_t compressed_size;
  const void* params;
  const void* cctx;
  const void* dctx;
};

void zstd_maybe_report() {
  const auto now = SteadyClock::now();
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          now.time_since_epoch())
                          .count();
  auto expected = zstd_next_report_ns.load(std::memory_order_relaxed);
  if (expected == 0) {
    zstd_next_report_ns.compare_exchange_strong(expected, now_ns + 1000000000LL,
                                                 std::memory_order_relaxed);
    return;
  }
  if (now_ns < expected ||
      !zstd_next_report_ns.compare_exchange_strong(
          expected, now_ns + 1000000000LL, std::memory_order_relaxed)) {
    return;
  }
  const auto calls = zstd_decompress_calls.exchange(0, std::memory_order_relaxed);
  const auto total = zstd_decompress_ns.exchange(0, std::memory_order_relaxed);
  const auto maximum = zstd_decompress_max_ns.exchange(0, std::memory_order_relaxed);
  const auto compressed =
      zstd_compressed_bytes.exchange(0, std::memory_order_relaxed);
  const auto uncompressed =
      zstd_uncompressed_bytes.exchange(0, std::memory_order_relaxed);
  if (calls != 0) {
    std::fprintf(stderr,
                 "nuah zstd: decompress_calls=%llu total_us=%llu avg_us=%llu "
                 "max_us=%llu compressed_bytes=%llu uncompressed_bytes=%llu\n",
                 calls, total / 1000ULL, (total / calls) / 1000ULL,
                 maximum / 1000ULL, compressed, uncompressed);
  }
}

void zstd_log_slow_stack(unsigned long long elapsed_ns) {
  if (!zstd_trace_stack_enabled() || elapsed_ns < zstd_trace_slow_threshold_ns())
    return;
  void* frames[16]{};
  const int count = ::backtrace(frames, static_cast<int>(std::size(frames)));
  std::fprintf(stderr, "nuah zstd: slow_decode_us=%llu callers=",
               elapsed_ns / 1000ULL);
  bool first = true;
  for (int index = 1; index < count; ++index) {
    Dl_info info{};
    if (::dladdr(frames[index], &info) == 0 || !info.dli_fname ||
        !std::strstr(info.dli_fname, "libroblox.so") || !info.dli_fbase)
      continue;
    const auto offset = reinterpret_cast<std::uintptr_t>(frames[index]) -
                        reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    std::fprintf(stderr, "%s0x%llx", first ? "" : ",",
                 static_cast<unsigned long long>(offset));
    first = false;
  }
  if (first) {
    std::fprintf(stderr, "raw=");
    for (int index = 1; index < count; ++index) {
      std::fprintf(stderr, "%s0x%llx", index == 1 ? "" : ",",
                   static_cast<unsigned long long>(
                       reinterpret_cast<std::uintptr_t>(frames[index])));
    }
  }
  std::fprintf(stderr, "\n");
}
}

extern "C" {
struct NuahConfiguration {
  int sdk_version = 36;
  int screen_width_dp = 1280;
  int screen_height_dp = 720;
  int screen_size = 2;  // ACONFIGURATION_SCREENSIZE_NORMAL
  int nav_hidden = 2;   // ACONFIGURATION_NAVHIDDEN_YES
};
/* The NDK declares these as exported `const char*` variables.  Exporting
 * arrays happens to satisfy a host ELF lookup but gives Android relocators a
 * six-byte object where they expect an eight-byte pointer, so apkenv rejects
 * the provider while resolving libroblox.so. */
const char* AMEDIAFORMAT_KEY_MIME = "mime";
const char* AMEDIAFORMAT_KEY_WIDTH = "width";
const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char* AMEDIAFORMAT_KEY_STRIDE = "stride";
const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
static int sl_engine, sl_android_configuration, sl_buffer_queue, sl_volume, sl_play, sl_android_simple_buffer_queue, sl_record;
void* SL_IID_ENGINE = &sl_engine;
void* SL_IID_ANDROIDCONFIGURATION = &sl_android_configuration;
void* SL_IID_BUFFERQUEUE = &sl_buffer_queue;
void* SL_IID_VOLUME = &sl_volume;
void* SL_IID_PLAY = &sl_play;
void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &sl_android_simple_buffer_queue;
void* SL_IID_RECORD = &sl_record;

void* AConfiguration_new() { return new NuahConfiguration; }
void AConfiguration_delete(void* configuration) { delete static_cast<NuahConfiguration*>(configuration); }
void AConfiguration_fromAssetManager(void*, void*) {}
void AConfiguration_getCountry(void*, char country[2]) {
  if (country) {
    country[0] = 'U';
    country[1] = 'S';
  }
}
void AConfiguration_getLanguage(void*, char language[2]) {
  if (language) {
    language[0] = 'e';
    language[1] = 'n';
  }
}
int AConfiguration_getNavHidden(void* configuration) {
  (void)configuration;
  // Keep the neutral value used by the working Sober-compatible path.  A
  // fabricated navigation-bar state changes Roblox's display/inset math.
  return 0;
}
/* Roblox gates its Vulkan path on the NDK configuration SDK value.  The
 * Java-side GameActivity already reports API 36; expose the same value from
 * the native libandroid façade instead of returning the old NDK sentinel. */
int AConfiguration_getSdkVersion(void* configuration) {
  return configuration ? static_cast<NuahConfiguration*>(configuration)->sdk_version : 36;
}
int AConfiguration_getScreenHeightDp(void* configuration) {
  return configuration ? static_cast<NuahConfiguration*>(configuration)->screen_height_dp : 720;
}
int AConfiguration_getScreenSize(void* configuration) {
  (void)configuration;
  return 0;
}
int AConfiguration_getScreenWidthDp(void* configuration) {
  return configuration ? static_cast<NuahConfiguration*>(configuration)->screen_width_dp : 1280;
}

void ANativeWindow_acquire(void* window) {
  if (looper_trace_enabled())
    std::fprintf(stderr, "nuah android: ANativeWindow_acquire(%p)\n", window);
  nuah_native_window_acquire(static_cast<NuahNativeWindow*>(window));
}
void ANativeWindow_release(void* window) {
  if (looper_trace_enabled())
    std::fprintf(stderr, "nuah android: ANativeWindow_release(%p)\n", window);
  nuah_native_window_release(static_cast<NuahNativeWindow*>(window));
}
void* ANativeWindow_fromSurface(void*, void* surface) {
  auto* window = nuah_native_window_from_surface(surface);
  if (!window) window = nuah_native_window_default();
  if (looper_trace_enabled())
    std::fprintf(stderr, "nuah android: ANativeWindow_fromSurface(surface=%p) -> %p\n",
                 surface, static_cast<void*>(window));
  /* Return the façade itself.  The EGL adapter uses the façade to obtain the
   * SDL/Wayland native handle; returning the raw wl_egl_window here makes ATL
   * interpret Wayland memory as an Android ANativeWindow and crash during
   * eglCreateWindowSurface. */
  return window;
}
int ANativeWindow_getWidth(void* window) {
  if (NUAH_UNLIKELY(looper_trace_enabled()))
    std::fprintf(stderr, "nuah android: ANativeWindow_getWidth(%p)\n", window);
  return nuah_native_window_width(static_cast<NuahNativeWindow*>(window));
}
int ANativeWindow_getHeight(void* window) {
  if (NUAH_UNLIKELY(looper_trace_enabled()))
    std::fprintf(stderr, "nuah android: ANativeWindow_getHeight(%p)\n", window);
  return nuah_native_window_height(static_cast<NuahNativeWindow*>(window));
}

void* AMediaCodec_createDecoderByType(const char*) { unsupported_media("AMediaCodec_createDecoderByType"); return nullptr; }
void* AMediaCodec_createEncoderByType(const char*) { unsupported_media("AMediaCodec_createEncoderByType"); return nullptr; }
int AMediaCodec_configure(void*, void*, void*, void*, int) { unsupported_media("AMediaCodec_configure"); return -1; }
int AMediaCodec_start(void*) { unsupported_media("AMediaCodec_start"); return -1; }
int AMediaCodec_stop(void*) { unsupported_media("AMediaCodec_stop"); return -1; }
int AMediaCodec_flush(void*) { unsupported_media("AMediaCodec_flush"); return -1; }
void AMediaCodec_delete(void*) { unsupported_media("AMediaCodec_delete"); }
ssize_t AMediaCodec_dequeueInputBuffer(void*, long) { unsupported_media("AMediaCodec_dequeueInputBuffer"); return -1; }
ssize_t AMediaCodec_dequeueOutputBuffer(void*, void*, long) { unsupported_media("AMediaCodec_dequeueOutputBuffer"); return -1; }
uint8_t* AMediaCodec_getInputBuffer(void*, size_t, size_t*) { unsupported_media("AMediaCodec_getInputBuffer"); return nullptr; }
uint8_t* AMediaCodec_getOutputBuffer(void*, size_t, size_t*) { unsupported_media("AMediaCodec_getOutputBuffer"); return nullptr; }
void* AMediaCodec_getOutputFormat(void*) { unsupported_media("AMediaCodec_getOutputFormat"); return nullptr; }
int AMediaCodec_queueInputBuffer(void*, size_t, size_t, size_t, long, uint32_t) { unsupported_media("AMediaCodec_queueInputBuffer"); return -1; }
int AMediaCodec_releaseOutputBuffer(void*, size_t, bool) { unsupported_media("AMediaCodec_releaseOutputBuffer"); return -1; }

void* AMediaFormat_new() { unsupported_media("AMediaFormat_new"); return nullptr; }
void AMediaFormat_delete(void*) { unsupported_media("AMediaFormat_delete"); }
int AMediaFormat_getBuffer(void*, const char*, void**, size_t*) { unsupported_media("AMediaFormat_getBuffer"); return -1; }
int AMediaFormat_getInt32(void*, const char*, int32_t*) { unsupported_media("AMediaFormat_getInt32"); return -1; }
void AMediaFormat_setBuffer(void*, const char*, const void*, size_t) { unsupported_media("AMediaFormat_setBuffer"); }
void AMediaFormat_setFloat(void*, const char*, float) { unsupported_media("AMediaFormat_setFloat"); }
void AMediaFormat_setInt32(void*, const char*, int32_t) { unsupported_media("AMediaFormat_setInt32"); }
void AMediaFormat_setString(void*, const char*, const char*) { unsupported_media("AMediaFormat_setString"); }
const char* AMediaFormat_toString(void*) { unsupported_media("AMediaFormat_toString"); return nullptr; }

int slCreateEngine(void**, unsigned, const void*, unsigned, const void*, const bool*) {
  unsupported_audio("slCreateEngine");
  return 12;  // SL_RESULT_FEATURE_UNSUPPORTED
}
void ZSTD_trace_compress_begin(...) {}
void ZSTD_trace_compress_end(...) {}
unsigned long long ZSTD_trace_decompress_begin(const void*) {
  if (NUAH_LIKELY(!zstd_trace_enabled())) return 0;
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          SteadyClock::now().time_since_epoch())
                          .count();
  return now_ns > 0 ? static_cast<unsigned long long>(now_ns) : 1ULL;
}
void ZSTD_trace_decompress_end(unsigned long long begin_ns,
                               const ZstdTrace* trace) {
  if (NUAH_LIKELY(!zstd_trace_enabled()) || begin_ns == 0) {
    return;
  }
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          SteadyClock::now().time_since_epoch())
                          .count();
  const auto elapsed = now_ns > 0 && static_cast<unsigned long long>(now_ns) >= begin_ns
                           ? static_cast<unsigned long long>(now_ns) - begin_ns
                           : 0ULL;
  zstd_decompress_calls.fetch_add(1, std::memory_order_relaxed);
  zstd_decompress_ns.fetch_add(elapsed, std::memory_order_relaxed);
  if (trace) {
    zstd_compressed_bytes.fetch_add(trace->compressed_size,
                                    std::memory_order_relaxed);
    zstd_uncompressed_bytes.fetch_add(trace->uncompressed_size,
                                      std::memory_order_relaxed);
  }
  zstd_log_slow_stack(elapsed);
  auto previous = zstd_decompress_max_ns.load(std::memory_order_relaxed);
  while (previous < elapsed &&
         !zstd_decompress_max_ns.compare_exchange_weak(
             previous, elapsed,
             std::memory_order_relaxed)) {
  }
  zstd_maybe_report();
}
void __gcov_dump() {}
void __gcov_flush() {}
// Roblox registers this purchase callback through its Java bridge. Nuah's
// native-only Runtime has no purchase service, so the callback is a safe no-op.
void Java_com_roblox_client_purchase_IAPPurchaseManager_nativeFinishPaymentsProtocolPurchaseWithReturn(void*, void*, ...) {}
}  // extern "C"
