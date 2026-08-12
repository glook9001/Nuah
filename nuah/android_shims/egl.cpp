#include "nuah/native_window_bridge.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <SDL3/SDL.h>

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {
struct LazyLibrary {
  std::atomic<void*> handle{nullptr};
  std::atomic<unsigned> state{0};
};

void* load_library(LazyLibrary& library, const char* name) {
  if (void* handle = library.handle.load(std::memory_order_acquire))
    return handle;
  unsigned expected = 0;
  if (!library.state.compare_exchange_strong(expected, 1,
                                             std::memory_order_acq_rel))
    return nullptr;
  void* handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
  library.handle.store(handle, std::memory_order_release);
  library.state.store(2, std::memory_order_release);
  return handle;
}

void* host_egl() {
  static LazyLibrary library;
  return load_library(library, "libEGL.so.1");
}

void* host_gles() {
  static LazyLibrary library;
  return load_library(library, "libGLESv2.so.2");
}

template <typename Function>
Function host_egl_function(const char* name) {
  return reinterpret_cast<Function>(host_egl() ? ::dlsym(host_egl(), name)
                                                : nullptr);
}

template <typename Function>
Function host_gles_function(const char* name) {
  return reinterpret_cast<Function>(host_gles() ? ::dlsym(host_gles(), name)
                                                 : nullptr);
}

SDL_Window* host_window(NuahNativeWindow* window) {
  return window ? static_cast<SDL_Window*>(nuah_native_window_host(window))
                : nullptr;
}

EGLDisplay display_for_window(NuahNativeWindow* window) {
  using GetPlatformDisplay = EGLDisplay (*)(EGLenum, void*, const EGLAttrib*);
  const auto get_platform = host_egl_function<GetPlatformDisplay>(
      "eglGetPlatformDisplay");
  if (!get_platform) return EGL_NO_DISPLAY;

  SDL_Window* sdl_window = host_window(window);
  if (!sdl_window) return EGL_NO_DISPLAY;
  const SDL_PropertiesID properties = SDL_GetWindowProperties(sdl_window);
  if (void* wayland = SDL_GetPointerProperty(
          properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr))
    return get_platform(EGL_PLATFORM_WAYLAND_KHR, wayland, nullptr);
  if (void* x11 = SDL_GetPointerProperty(
          properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr))
    return get_platform(EGL_PLATFORM_X11_KHR, x11, nullptr);
  return EGL_NO_DISPLAY;
}

std::mutex surfaces_mutex;
std::unordered_map<EGLSurface, NuahNativeWindow*> surfaces;

bool owns_surface(EGLSurface surface) {
  if (!surface) return false;
  std::scoped_lock lock(surfaces_mutex);
  return surfaces.find(surface) != surfaces.end();
}

/* ATL's Java EGL implementation creates an ATLSurface object rather than a
 * host EGLSurface.  Nuah's libandroid provider is intentionally local, while
 * ATL's companion libandroid.so.0 is global, so RTLD_DEFAULT is the narrow
 * escape hatch for those foreign surfaces.  Never use it for a Nuah-owned
 * surface: the host Mesa calls below are the correct ABI there. */
template <typename Function>
Function atl_egl_function(const char* name) {
  return reinterpret_cast<Function>(::dlsym(RTLD_DEFAULT, name));
}

struct EglPerfMetrics {
  std::mutex mutex;
  uint64_t count = 0;
  uint64_t interval_total_ns = 0;
  uint64_t interval_max_ns = 0;
  uint64_t call_total_ns = 0;
  uint64_t call_max_ns = 0;
  uint64_t previous_ns = 0;
  uint64_t next_report_ns = 0;
};

EglPerfMetrics& egl_perf_metrics() {
  static EglPerfMetrics value;
  return value;
}

bool egl_perf_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("NUAH_PERF_TRACE");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

uint64_t egl_monotonic_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void record_egl_swap(uint64_t started_ns, uint64_t finished_ns) {
  if (!egl_perf_enabled()) return;
  auto& metrics = egl_perf_metrics();
  std::scoped_lock lock(metrics.mutex);
  ++metrics.count;
  const uint64_t call_ns = finished_ns - started_ns;
  metrics.call_total_ns += call_ns;
  metrics.call_max_ns = std::max(metrics.call_max_ns, call_ns);
  if (metrics.previous_ns != 0) {
    const uint64_t interval_ns = finished_ns - metrics.previous_ns;
    metrics.interval_total_ns += interval_ns;
    metrics.interval_max_ns = std::max(metrics.interval_max_ns, interval_ns);
  }
  metrics.previous_ns = finished_ns;
  if (metrics.next_report_ns == 0)
    metrics.next_report_ns = finished_ns + 1000000000ULL;
  if (finished_ns < metrics.next_report_ns) return;
  const uint64_t intervals = metrics.count > 1 ? metrics.count - 1 : 0;
  std::fprintf(stderr,
               "nuah perf: egl swaps=%llu avg_interval_us=%llu "
               "max_interval_us=%llu avg_call_us=%llu max_call_us=%llu\n",
               static_cast<unsigned long long>(metrics.count),
               static_cast<unsigned long long>(
                   intervals ? metrics.interval_total_ns / intervals / 1000ULL : 0),
               static_cast<unsigned long long>(metrics.interval_max_ns / 1000ULL),
               static_cast<unsigned long long>(metrics.call_total_ns /
                                               metrics.count / 1000ULL),
               static_cast<unsigned long long>(metrics.call_max_ns / 1000ULL));
  metrics.count = 0;
  metrics.interval_total_ns = 0;
  metrics.interval_max_ns = 0;
  metrics.call_total_ns = 0;
  metrics.call_max_ns = 0;
  metrics.previous_ns = 0;
  metrics.next_report_ns = finished_ns + 1000000000ULL;
}

bool egl_trace_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("NUAH_EGL_TRACE");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

template <typename Function>
Function require_egl(const char* name) {
  return host_egl_function<Function>(name);
}
}  // namespace

extern "C" {

EGLDisplay bionic_eglGetDisplay(EGLNativeDisplayType) {
  NuahNativeWindow* window = nuah_native_window_default();
  EGLDisplay display = display_for_window(window);
  if (window) nuah_native_window_release(window);
  if (display != EGL_NO_DISPLAY) return display;
  const auto get_display = require_egl<EGLDisplay (*)(EGLNativeDisplayType)>(
      "eglGetDisplay");
  return get_display ? get_display(EGL_DEFAULT_DISPLAY) : EGL_NO_DISPLAY;
}

EGLBoolean bionic_eglChooseConfig(EGLDisplay display, EGLint* attributes,
                                  EGLConfig* configs, EGLint config_size,
                                  EGLint* count) {
  const auto function = require_egl<PFNEGLCHOOSECONFIGPROC>("eglChooseConfig");
  return function ? function(display, attributes, configs, config_size, count)
                  : EGL_FALSE;
}

EGLSurface bionic_eglCreatePbufferSurface(EGLDisplay display, EGLConfig config,
                                          const EGLint* attributes) {
  const auto function = require_egl<PFNEGLCREATEPBUFFERSURFACEPROC>(
      "eglCreatePbufferSurface");
  return function ? function(display, config, attributes) : EGL_NO_SURFACE;
}

EGLSurface bionic_eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                          NuahNativeWindow* window,
                                          const EGLint* attributes) {
  if (!window) return EGL_NO_SURFACE;
  const auto function = require_egl<PFNEGLCREATEWINDOWSURFACEPROC>(
      "eglCreateWindowSurface");
  const EGLNativeWindowType native_window = reinterpret_cast<EGLNativeWindowType>(
      nuah_native_window_egl_handle(window));
  if (!function || !native_window) return EGL_NO_SURFACE;
  const EGLSurface surface =
      function(display, config, native_window, attributes);
  if (surface == EGL_NO_SURFACE) return surface;
  nuah_native_window_acquire(window);
  std::scoped_lock lock(surfaces_mutex);
  surfaces.emplace(surface, window);
  return surface;
}

EGLBoolean bionic_eglDestroySurface(EGLDisplay display, EGLSurface surface) {
  if (!owns_surface(surface)) {
    using Destroy = EGLBoolean (*)(EGLDisplay, EGLSurface);
    if (const auto atl = atl_egl_function<Destroy>(
            "bionic_eglDestroySurface"))
      return atl(display, surface);
  }
  const auto function = require_egl<PFNEGLDESTROYSURFACEPROC>(
      "eglDestroySurface");
  const EGLBoolean result = function ? function(display, surface) : EGL_FALSE;
  NuahNativeWindow* window = nullptr;
  {
    std::scoped_lock lock(surfaces_mutex);
    const auto found = surfaces.find(surface);
    if (found != surfaces.end()) {
      window = found->second;
      surfaces.erase(found);
    }
  }
  if (window) nuah_native_window_release(window);
  return result;
}

EGLBoolean bionic_eglMakeCurrent(EGLDisplay display, EGLSurface draw,
                                 EGLSurface read, EGLContext context) {
  if ((draw && !owns_surface(draw)) || (read && !owns_surface(read))) {
    using MakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface,
                                       EGLContext);
    if (const auto atl = atl_egl_function<MakeCurrent>(
            "bionic_eglMakeCurrent"))
      return atl(display, draw, read, context);
  }
  const auto function = require_egl<PFNEGLMAKECURRENTPROC>("eglMakeCurrent");
  return function ? function(display, draw, read, context) : EGL_FALSE;
}

EGLBoolean bionic_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
  if (!owns_surface(surface)) {
    using SwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
    if (const auto atl = atl_egl_function<SwapBuffers>(
            "bionic_eglSwapBuffers"))
      return atl(display, surface);
  }
  const auto function = require_egl<PFNEGLSWAPBUFFERSPROC>("eglSwapBuffers");
  static unsigned long calls = 0;
  const uint64_t started_ns = egl_perf_enabled() ? egl_monotonic_ns() : 0;
  const EGLBoolean result = function ? function(display, surface) : EGL_FALSE;
  if (egl_perf_enabled()) record_egl_swap(started_ns, egl_monotonic_ns());
  if (egl_trace_enabled() && calls++ < 20) {
    std::fprintf(stderr, "nuah egl: swap surface=%p result=%d\n",
                 static_cast<void*>(surface), result == EGL_TRUE ? 1 : 0);
  }
  return result;
}

EGLBoolean bionic_eglQuerySurface(EGLDisplay display, EGLSurface surface,
                                  EGLint attribute, EGLint* value) {
  if (!owns_surface(surface)) {
    using QuerySurface = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint,
                                        EGLint*);
    if (const auto atl = atl_egl_function<QuerySurface>(
            "bionic_eglQuerySurface")) {
      const EGLBoolean result = atl(display, surface, attribute, value);
      if (result == EGL_TRUE ||
          (attribute != EGL_WIDTH && attribute != EGL_HEIGHT))
        return result;
    }
    /* A foreign surface can be an ATL object whose query implementation is
     * unavailable during early bootstrap.  The active Nuah façade still has
     * the authoritative geometry; use it instead of passing an invalid
     * ATLSurface pointer to Mesa's eglQuerySurface. */
    if (value && (attribute == EGL_WIDTH || attribute == EGL_HEIGHT)) {
      NuahNativeWindow* window = nuah_native_window_default();
      if (window) {
        const int dimension = attribute == EGL_WIDTH
                                  ? nuah_native_window_width(window)
                                  : nuah_native_window_height(window);
        nuah_native_window_release(window);
        if (dimension > 0) {
          *value = dimension;
          return EGL_TRUE;
        }
      }
    }
  }
  const auto function = require_egl<PFNEGLQUERYSURFACEPROC>("eglQuerySurface");
  const EGLBoolean result =
      function ? function(display, surface, attribute, value) : EGL_FALSE;
  if (egl_trace_enabled() && value &&
      (attribute == EGL_WIDTH || attribute == EGL_HEIGHT) ) {
    std::fprintf(stderr, "nuah egl: query surface=%p attr=%s value=%d ok=%d\n",
                 static_cast<void*>(surface),
                 attribute == EGL_WIDTH ? "width" : "height", *value,
                 result == EGL_TRUE ? 1 : 0);
  }
  return result;
}

EGLSurface bionic_eglGetCurrentSurface(EGLint readdraw) {
  const auto function = require_egl<PFNEGLGETCURRENTSURFACEPROC>(
      "eglGetCurrentSurface");
  return function ? function(readdraw) : EGL_NO_SURFACE;
}

void (*bionic_eglGetProcAddress(const char* name))(void) {
  if (!name) return nullptr;
  if (std::strcmp(name, "eglGetDisplay") == 0)
    return reinterpret_cast<void (*)(void)>(&bionic_eglGetDisplay);
  if (std::strcmp(name, "eglCreateWindowSurface") == 0)
    return reinterpret_cast<void (*)(void)>(&bionic_eglCreateWindowSurface);
  if (std::strcmp(name, "eglDestroySurface") == 0)
    return reinterpret_cast<void (*)(void)>(&bionic_eglDestroySurface);
  if (std::strcmp(name, "eglMakeCurrent") == 0)
    return reinterpret_cast<void (*)(void)>(&bionic_eglMakeCurrent);
  if (std::strcmp(name, "eglSwapBuffers") == 0)
    return reinterpret_cast<void (*)(void)>(&bionic_eglSwapBuffers);
  if (std::strcmp(name, "eglQuerySurface") == 0)
    return reinterpret_cast<void (*)(void)>(&bionic_eglQuerySurface);
  using GetProcAddress = void (*(*)(const char*))(void);
  const auto function = require_egl<GetProcAddress>("eglGetProcAddress");
  return function ? function(name) : nullptr;
}

EGLBoolean bionic_eglPresentationTimeANDROID(EGLDisplay, EGLSurface,
                                             EGLnsecsANDROID) {
  return EGL_TRUE;
}

EGLImage bionic_eglCreateImageKHR(EGLDisplay display, EGLContext context,
                                  EGLenum target, EGLClientBuffer buffer,
                                  const EGLAttrib* attributes) {
  const auto function = require_egl<PFNEGLCREATEIMAGEPROC>("eglCreateImage");
  return function ? function(display, context, target, buffer, attributes)
                  : EGL_NO_IMAGE;
}

EGLBoolean bionic_eglDestroyImageKHR(EGLDisplay display, EGLImage image) {
  const auto function = require_egl<PFNEGLDESTROYIMAGEPROC>("eglDestroyImage");
  return function ? function(display, image) : EGL_FALSE;
}

void bionic_glBindFramebuffer(GLenum target, GLuint framebuffer) {
  using BindFramebuffer = void (*)(GLenum, GLuint);
  const auto function = host_gles_function<BindFramebuffer>("glBindFramebuffer");
  if (function) function(target, framebuffer);
}

}  // extern "C"
