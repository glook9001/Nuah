#include <dlfcn.h>
#include <execinfo.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include "nuah/android_abi_registry.h"
#include "nuah/native_window_bridge.h"
#ifndef NUAH_LIKELY
#define NUAH_LIKELY(x) (__builtin_expect(!!(x), 1))
#endif
#ifndef NUAH_UNLIKELY
#define NUAH_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif
namespace {
// Shared helper for env-flag getters: getenv cached in per-flag static, zero-alloc hot path.
inline bool env_enabled(const char* n) { const char* v = std::getenv(n); return v && *v && std::strcmp(v, "0") != 0; }
#define NUAH_ENV_FLAG(fn, name) bool fn() { static const bool e = env_enabled(name); return e; }
/* ALooper_pollOnce is hot when Roblox asks for non-blocking poll. Environment never changes, so do not getenv on every poll (or every ANativeWindow query) just to decide whether to log. */
NUAH_ENV_FLAG(looper_trace_enabled, "NUAH_BOOTSTRAP_TRACE") NUAH_ENV_FLAG(zstd_trace_enabled, "NUAH_ZSTD_TRACE") NUAH_ENV_FLAG(zstd_trace_stack_enabled, "NUAH_ZSTD_TRACE_STACK")
void unsupported_lib(const char* l, const char* s) { nuah_android_api_unsupported(l, s); errno = ENOSYS; }
inline void unsupported_media(const char* s) { unsupported_lib("libmediandk.so", s); } inline void unsupported_audio(const char* s) { unsupported_lib("libOpenSLES.so", s); }
unsigned long long zstd_trace_slow_threshold_ns() { static const unsigned long long t = [] { const char* r = std::getenv("NUAH_ZSTD_TRACE_SLOW_US"); if (!r || !*r) return 2000ULL; char* e = nullptr; auto p = std::strtoull(r, &e, 10); return e != r && *e == '\0' && p > 0 ? p : 2000ULL; }(); return t * 1000ULL; }
using SteadyClock = std::chrono::steady_clock;
std::atomic<unsigned long long> zstd_decompress_calls{0}, zstd_decompress_ns{0}, zstd_decompress_max_ns{0}, zstd_compressed_bytes{0}, zstd_uncompressed_bytes{0}; std::atomic<long long> zstd_next_report_ns{0};
/* Zstd trace interface is ABI-stable through `version`. Roblox statically links a trace-enabled Zstd and resolves these weak imports from Nuah's Android provider, so preserve layout instead of using ellipsis hooks. */
struct ZstdTrace { unsigned version; int streaming; unsigned dictionary_id; int dictionary_is_cold; std::size_t dictionary_size, uncompressed_size, compressed_size; const void *params, *cctx, *dctx; };
void zstd_maybe_report() {
  auto now = SteadyClock::now(); auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(); auto e = zstd_next_report_ns.load(std::memory_order_relaxed);
  if (e == 0) { zstd_next_report_ns.compare_exchange_strong(e, now_ns + 1000000000LL, std::memory_order_relaxed); return; }
  if (now_ns < e || !zstd_next_report_ns.compare_exchange_strong(e, now_ns + 1000000000LL, std::memory_order_relaxed)) return;
  auto calls = zstd_decompress_calls.exchange(0, std::memory_order_relaxed), total = zstd_decompress_ns.exchange(0, std::memory_order_relaxed), maximum = zstd_decompress_max_ns.exchange(0, std::memory_order_relaxed), comp = zstd_compressed_bytes.exchange(0, std::memory_order_relaxed), uncomp = zstd_uncompressed_bytes.exchange(0, std::memory_order_relaxed);
  if (calls != 0) std::fprintf(stderr, "nuah zstd: decompress_calls=%llu total_us=%llu avg_us=%llu max_us=%llu compressed_bytes=%llu uncompressed_bytes=%llu\n", calls, total / 1000ULL, (total / calls) / 1000ULL, maximum / 1000ULL, comp, uncomp); }
void zstd_log_slow_stack(unsigned long long elapsed_ns) {
  if (!zstd_trace_stack_enabled() || elapsed_ns < zstd_trace_slow_threshold_ns()) return;
  void* frames[16]{}; int n = ::backtrace(frames, static_cast<int>(std::size(frames)));
  std::fprintf(stderr, "nuah zstd: slow_decode_us=%llu callers=", elapsed_ns / 1000ULL); bool first = true;
  for (int i = 1; i < n; ++i) { Dl_info inf{}; if (::dladdr(frames[i], &inf) == 0 || !inf.dli_fname || !std::strstr(inf.dli_fname, "libroblox.so") || !inf.dli_fbase) continue; auto off = reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(inf.dli_fbase); std::fprintf(stderr, "%s0x%llx", first ? "" : ",", static_cast<unsigned long long>(off)); first = false; }
  if (first) { std::fprintf(stderr, "raw="); for (int i = 1; i < n; ++i) std::fprintf(stderr, "%s0x%llx", i == 1 ? "" : ",", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[i]))); }
  std::fprintf(stderr, "\n"); }
}  // namespace
extern "C" {
struct NuahConfiguration { int sdk_version = 36, screen_width_dp = 1280, screen_height_dp = 720; };
/* NDK declares these as exported `const char*` variables. Exporting arrays satisfies host ELF lookup but gives Android relocators a six-byte object where they expect an eight-byte pointer, so apkenv rejects the provider while resolving libroblox.so. */
const char *AMEDIAFORMAT_KEY_MIME = "mime", *AMEDIAFORMAT_KEY_WIDTH = "width", *AMEDIAFORMAT_KEY_HEIGHT = "height", *AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format", *AMEDIAFORMAT_KEY_STRIDE = "stride";
const char *AMEDIAFORMAT_KEY_BIT_RATE = "bitrate", *AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate", *AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval", *AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count", *AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
static int sl_engine, sl_android_configuration, sl_buffer_queue, sl_volume, sl_play, sl_android_simple_buffer_queue, sl_record;
void *SL_IID_ENGINE = &sl_engine, *SL_IID_ANDROIDCONFIGURATION = &sl_android_configuration, *SL_IID_BUFFERQUEUE = &sl_buffer_queue, *SL_IID_VOLUME = &sl_volume, *SL_IID_PLAY = &sl_play, *SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &sl_android_simple_buffer_queue, *SL_IID_RECORD = &sl_record;
void* AConfiguration_new() { return new NuahConfiguration; } void AConfiguration_delete(void* c) { delete static_cast<NuahConfiguration*>(c); } void AConfiguration_fromAssetManager(void*, void*) {}
inline void fill_locale2(char o[2], char a, char b) { if (o) { o[0] = a; o[1] = b; } } void AConfiguration_getCountry(void*, char c[2]) { fill_locale2(c, 'U', 'S'); } void AConfiguration_getLanguage(void*, char l[2]) { fill_locale2(l, 'e', 'n'); }
int AConfiguration_getNavHidden(void*) { /* Keep neutral value. Fabricated nav-bar state changes Roblox display/inset math. */ return 0; }
/* Roblox gates Vulkan path on NDK configuration SDK value. Java-side GameActivity already reports API 36; expose same value from native libandroid façade instead of returning old NDK sentinel. */
int AConfiguration_getSdkVersion(void* c) { return c ? static_cast<NuahConfiguration*>(c)->sdk_version : 36; } int AConfiguration_getScreenHeightDp(void* c) { return c ? static_cast<NuahConfiguration*>(c)->screen_height_dp : 720; }
int AConfiguration_getScreenSize(void*) { return 0; } int AConfiguration_getScreenWidthDp(void* c) { return c ? static_cast<NuahConfiguration*>(c)->screen_width_dp : 1280; }
// Shared trace helper: single env-gated fprintf keeps ANativeWindow shims dense.
#define NUAH_WINDOW_TRACE(...) if (looper_trace_enabled()) std::fprintf(stderr, __VA_ARGS__)
void ANativeWindow_acquire(void* w) { NUAH_WINDOW_TRACE("nuah android: ANativeWindow_acquire(%p)\n", w); nuah_native_window_acquire(static_cast<NuahNativeWindow*>(w)); }
void ANativeWindow_release(void* w) { NUAH_WINDOW_TRACE("nuah android: ANativeWindow_release(%p)\n", w); nuah_native_window_release(static_cast<NuahNativeWindow*>(w)); }
void* ANativeWindow_fromSurface(void*, void* s) { auto* w = nuah_native_window_from_surface(s); if (!w) w = nuah_native_window_default(); NUAH_WINDOW_TRACE("nuah android: ANativeWindow_fromSurface(surface=%p) -> %p\n", s, static_cast<void*>(w)); /* Return façade itself. EGL adapter uses façade for SDL/Wayland handle; returning raw wl_egl_window makes ATL interpret Wayland memory as ANativeWindow and crash in eglCreateWindowSurface. */ return w; }
int ANativeWindow_getWidth(void* w) { if (NUAH_UNLIKELY(looper_trace_enabled())) std::fprintf(stderr, "nuah android: ANativeWindow_getWidth(%p)\n", w); return nuah_native_window_width(static_cast<NuahNativeWindow*>(w)); } int ANativeWindow_getHeight(void* w) { if (NUAH_UNLIKELY(looper_trace_enabled())) std::fprintf(stderr, "nuah android: ANativeWindow_getHeight(%p)\n", w); return nuah_native_window_height(static_cast<NuahNativeWindow*>(w)); }
#define S(ret, val, name, ...) ret name(__VA_ARGS__) { unsupported_media(#name); return val; }
#define V(name, ...) void name(__VA_ARGS__) { unsupported_media(#name); }
// Table of unsupported libmediandk stubs (symbol names must match version script).
S(void*, nullptr, AMediaCodec_createDecoderByType, const char*) S(void*, nullptr, AMediaCodec_createEncoderByType, const char*) S(int, -1, AMediaCodec_configure, void*, void*, void*, void*, int) S(int, -1, AMediaCodec_start, void*) S(int, -1, AMediaCodec_stop, void*) S(int, -1, AMediaCodec_flush, void*)
V(AMediaCodec_delete, void*) S(ssize_t, -1, AMediaCodec_dequeueInputBuffer, void*, long) S(ssize_t, -1, AMediaCodec_dequeueOutputBuffer, void*, void*, long) S(uint8_t*, nullptr, AMediaCodec_getInputBuffer, void*, size_t, size_t*) S(uint8_t*, nullptr, AMediaCodec_getOutputBuffer, void*, size_t, size_t*) S(void*, nullptr, AMediaCodec_getOutputFormat, void*)
S(int, -1, AMediaCodec_queueInputBuffer, void*, size_t, size_t, size_t, long, uint32_t) S(int, -1, AMediaCodec_releaseOutputBuffer, void*, size_t, bool) S(void*, nullptr, AMediaFormat_new) V(AMediaFormat_delete, void*) S(int, -1, AMediaFormat_getBuffer, void*, const char*, void**, size_t*) S(int, -1, AMediaFormat_getInt32, void*, const char*, int32_t*)
V(AMediaFormat_setBuffer, void*, const char*, const void*, size_t) V(AMediaFormat_setFloat, void*, const char*, float) V(AMediaFormat_setInt32, void*, const char*, int32_t) V(AMediaFormat_setString, void*, const char*, const char*) S(const char*, nullptr, AMediaFormat_toString, void*)
int slCreateEngine(void**, unsigned, const void*, unsigned, const void*, const bool*) { unsupported_audio("slCreateEngine"); return 12; /* SL_RESULT_FEATURE_UNSUPPORTED */ }
void ZSTD_trace_compress_begin(...) {} void ZSTD_trace_compress_end(...) {}
unsigned long long ZSTD_trace_decompress_begin(const void*) { if (NUAH_LIKELY(!zstd_trace_enabled())) return 0; auto n = std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count(); return n > 0 ? static_cast<unsigned long long>(n) : 1ULL; }
void ZSTD_trace_decompress_end(unsigned long long b, const ZstdTrace* t) {
  if (NUAH_LIKELY(!zstd_trace_enabled()) || b == 0) return;
  auto n = std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count(); auto e = n > 0 && static_cast<unsigned long long>(n) >= b ? static_cast<unsigned long long>(n) - b : 0ULL;
  zstd_decompress_calls.fetch_add(1, std::memory_order_relaxed); zstd_decompress_ns.fetch_add(e, std::memory_order_relaxed);
  if (t) { zstd_compressed_bytes.fetch_add(t->compressed_size, std::memory_order_relaxed); zstd_uncompressed_bytes.fetch_add(t->uncompressed_size, std::memory_order_relaxed); }
  zstd_log_slow_stack(e); auto p = zstd_decompress_max_ns.load(std::memory_order_relaxed); while (p < e && !zstd_decompress_max_ns.compare_exchange_weak(p, e, std::memory_order_relaxed)) {} zstd_maybe_report(); }
// Roblox registers purchase callback through Java bridge. Native-only Runtime has no purchase service, so callback is safe no-op.
void Java_com_roblox_client_purchase_IAPPurchaseManager_nativeFinishPaymentsProtocolPurchaseWithReturn(void*, void*, ...) {}
}  // extern "C"
