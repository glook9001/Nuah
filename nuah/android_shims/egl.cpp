#include "nuah/native_window_bridge.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <SDL3/SDL.h>

#include <dlfcn.h>

#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {
void* host_egl() {
  static void* handle = ::dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  return handle;
}

void* host_gles() {
  static void* handle = ::dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
  return handle;
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
  const auto function = require_egl<PFNEGLMAKECURRENTPROC>("eglMakeCurrent");
  return function ? function(display, draw, read, context) : EGL_FALSE;
}

EGLBoolean bionic_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
  const auto function = require_egl<PFNEGLSWAPBUFFERSPROC>("eglSwapBuffers");
  static unsigned long calls = 0;
  const EGLBoolean result = function ? function(display, surface) : EGL_FALSE;
  if (egl_trace_enabled() && calls++ < 20) {
    std::fprintf(stderr, "nuah egl: swap surface=%p result=%d\n",
                 static_cast<void*>(surface), result == EGL_TRUE ? 1 : 0);
  }
  return result;
}

EGLBoolean bionic_eglQuerySurface(EGLDisplay display, EGLSurface surface,
                                  EGLint attribute, EGLint* value) {
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
