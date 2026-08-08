#include "nuah/native_window_bridge.h"
#include "nuah/android_abi_registry.h"

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <poll.h>
#include <vector>
#include <unistd.h>

namespace {
void unsupported_media(const char* symbol) {
  nuah_android_api_unsupported("libmediandk.so", symbol);
  errno = ENOSYS;
}
void unsupported_audio(const char* symbol) {
  nuah_android_api_unsupported("libOpenSLES.so", symbol);
  errno = ENOSYS;
}
}

extern "C" {
using NuahLooperCallback = int (*)(int, int, void*);
struct NuahLooperRegistration {
  int fd;
  int ident;
  int events;
  NuahLooperCallback callback;
  void* data;
};
struct NuahLooper {
  int refs = 1;
  unsigned int polls = 0;
  std::mutex mutex;
  std::vector<NuahLooperRegistration> registrations;
};
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

void* ALooper_forThread() { static thread_local NuahLooper looper; return &looper; }
void* ALooper_prepare(int) { return ALooper_forThread(); }
void ALooper_acquire(void* looper) { if (looper) ++static_cast<NuahLooper*>(looper)->refs; }
void ALooper_release(void* looper) { if (looper && static_cast<NuahLooper*>(looper)->refs > 1) --static_cast<NuahLooper*>(looper)->refs; }
int ALooper_addFd(void* opaque_looper, int fd, int ident, int events,
                  NuahLooperCallback callback, void* data) {
  if (!opaque_looper || fd < 0 || (!callback && ident < 0)) {
    errno = EINVAL;
    return -1;
  }
  auto* looper = static_cast<NuahLooper*>(opaque_looper);
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace) {
    std::fprintf(stderr,
                 "nuah looper: add thread=%ld looper=%p fd=%d ident=%d "
                 "events=%d callback=%p\n",
                 static_cast<long>(::gettid()), opaque_looper, fd, ident,
                 events, reinterpret_cast<void*>(callback));
  }
  std::scoped_lock lock(looper->mutex);
  for (auto& registration : looper->registrations) {
    if (registration.fd == fd) {
      registration = {fd, ident, events, callback, data};
      return 1;
    }
  }
  looper->registrations.push_back({fd, ident, events, callback, data});
  return 1;
}
int ALooper_removeFd(void* opaque_looper, int fd) {
  if (!opaque_looper) return 0;
  auto* looper = static_cast<NuahLooper*>(opaque_looper);
  std::scoped_lock lock(looper->mutex);
  for (auto it = looper->registrations.begin();
       it != looper->registrations.end(); ++it) {
    if (it->fd == fd) {
      looper->registrations.erase(it);
      return 1;
    }
  }
  return 0;
}
int ALooper_pollOnce(int timeout_ms, int* out_fd, int* out_events,
                     void** out_data) {
  auto* looper = static_cast<NuahLooper*>(ALooper_forThread());
  std::vector<NuahLooperRegistration> registrations;
  {
    std::scoped_lock lock(looper->mutex);
    registrations = looper->registrations;
  }
  if (registrations.empty()) {
    if (timeout_ms > 0) (void)::poll(nullptr, 0, timeout_ms);
    return -3;  // ALOOPER_POLL_TIMEOUT or no registered source.
  }
  std::vector<pollfd> descriptors;
  descriptors.reserve(registrations.size());
  for (const auto& registration : registrations) {
    short events = 0;
    if ((registration.events & 1) != 0) events |= POLLIN;
    if ((registration.events & 2) != 0) events |= POLLOUT;
    descriptors.push_back({registration.fd, events, 0});
  }
  const int ready = ::poll(descriptors.data(), descriptors.size(), timeout_ms);
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace && looper->polls++ < 12) {
    std::fprintf(stderr,
                 "nuah looper: poll thread=%ld looper=%p sources=%zu "
                 "timeout=%d ready=%d\n",
                 static_cast<long>(::gettid()), static_cast<void*>(looper),
                 registrations.size(), timeout_ms, ready);
  }
  if (ready == 0) return -3;  // ALOOPER_POLL_TIMEOUT
  if (ready < 0) return -4;   // ALOOPER_POLL_ERROR
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    if (!descriptors[index].revents) continue;
    int android_events = 0;
    if ((descriptors[index].revents & POLLIN) != 0) android_events |= 1;
    if ((descriptors[index].revents & POLLOUT) != 0) android_events |= 2;
    if ((descriptors[index].revents & POLLERR) != 0) android_events |= 4;
    if ((descriptors[index].revents & POLLHUP) != 0) android_events |= 8;
    if ((descriptors[index].revents & POLLNVAL) != 0) android_events |= 16;
    const auto registration = registrations[index];
    if (registration.callback) {
      if (!registration.callback(registration.fd, android_events,
                                 registration.data)) {
        (void)ALooper_removeFd(looper, registration.fd);
      }
      return -2;  // ALOOPER_POLL_CALLBACK
    }
    if (out_fd) *out_fd = registration.fd;
    if (out_events) *out_events = android_events;
    if (out_data) *out_data = registration.data;
    return registration.ident;
  }
  return -3;
}

void ANativeWindow_acquire(void* window) {
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr, "nuah android: ANativeWindow_acquire(%p)\n", window);
  nuah_native_window_acquire(static_cast<NuahNativeWindow*>(window));
}
void ANativeWindow_release(void* window) {
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr, "nuah android: ANativeWindow_release(%p)\n", window);
  nuah_native_window_release(static_cast<NuahNativeWindow*>(window));
}
void* ANativeWindow_fromSurface(void*, void* surface) {
  auto* window = nuah_native_window_from_surface(surface);
  if (!window) window = nuah_native_window_default();
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr, "nuah android: ANativeWindow_fromSurface(surface=%p) -> %p\n",
                 surface, static_cast<void*>(window));
  /* Return the façade itself.  The EGL adapter uses the façade to obtain the
   * SDL/Wayland native handle; returning the raw wl_egl_window here makes ATL
   * interpret Wayland memory as an Android ANativeWindow and crash during
   * eglCreateWindowSurface. */
  return window;
}
int ANativeWindow_getWidth(void* window) {
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr, "nuah android: ANativeWindow_getWidth(%p)\n", window);
  return nuah_native_window_width(static_cast<NuahNativeWindow*>(window));
}
int ANativeWindow_getHeight(void* window) {
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
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
void ZSTD_trace_decompress_begin(...) {}
void ZSTD_trace_decompress_end(...) {}
void __gcov_dump() {}
void __gcov_flush() {}
// Roblox registers this purchase callback through its Java bridge. Nuah's
// native-only Runtime has no purchase service, so the callback is a safe no-op.
void Java_com_roblox_client_purchase_IAPPurchaseManager_nativeFinishPaymentsProtocolPurchaseWithReturn(void*, void*, ...) {}
}  // extern "C"
