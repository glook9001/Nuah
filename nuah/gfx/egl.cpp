#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "nuah/native_window_bridge.h"
namespace {
struct LazyLibrary { std::atomic<void*> handle{nullptr}; std::atomic<unsigned> state{0}; };
void* load_library(LazyLibrary& l, const char* n) { if (void* h = l.handle.load(std::memory_order_acquire)) return h; unsigned e = 0; if (!l.state.compare_exchange_strong(e, 1, std::memory_order_acq_rel)) return nullptr; void* h = ::dlopen(n, RTLD_NOW | RTLD_LOCAL); l.handle.store(h, std::memory_order_release); l.state.store(2, std::memory_order_release); return h; }
void* host_egl() { static LazyLibrary l; return load_library(l, "libEGL.so.1"); } void* host_gles() { static LazyLibrary l; return load_library(l, "libGLESv2.so.2"); }
// Shared helper: EGL/GLES lookups are both null-guarded dlsym (zero-alloc, no logging).
template <typename F> F host_lookup(void* (*lib)(), const char* n) { void* h = lib(); return reinterpret_cast<F>(h ? ::dlsym(h, n) : nullptr); }
template <typename F> F host_egl_function(const char* n) { return host_lookup<F>(host_egl, n); } template <typename F> F host_gles_function(const char* n) { return host_lookup<F>(host_gles, n); }
SDL_Window* host_window(NuahNativeWindow* w) { return w ? static_cast<SDL_Window*>(nuah_native_window_host(w)) : nullptr; }
EGLDisplay display_for_window(NuahNativeWindow* w) {
  using GetPlatformDisplay = EGLDisplay (*)(EGLenum, void*, const EGLAttrib*);
  auto get_platform = host_egl_function<GetPlatformDisplay>("eglGetPlatformDisplay"); if (!get_platform) return EGL_NO_DISPLAY;
  SDL_Window* s = host_window(w); if (!s) return EGL_NO_DISPLAY;
  SDL_PropertiesID p = SDL_GetWindowProperties(s); // Collapsed Wayland/X11 fallback: ordered table, same priority.
  static const struct { const char* key; EGLenum platform; } kDisplays[] = {{SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, EGL_PLATFORM_WAYLAND_KHR}, {SDL_PROP_WINDOW_X11_DISPLAY_POINTER, EGL_PLATFORM_X11_KHR}};
  for (const auto& q : kDisplays) if (void* v = SDL_GetPointerProperty(p, q.key, nullptr)) return get_platform(q.platform, v, nullptr);
  return EGL_NO_DISPLAY; }
std::mutex surfaces_mutex; std::unordered_map<EGLSurface, NuahNativeWindow*> surfaces;
bool owns_surface(EGLSurface s) { if (!s) return false; std::scoped_lock l(surfaces_mutex); return surfaces.find(s) != surfaces.end(); }
/* ATL's Java EGL creates an ATLSurface rather than a host EGLSurface. Nuah's libandroid provider is intentionally local, while ATL's companion libandroid.so.0 is global, so RTLD_DEFAULT is the narrow escape hatch for those foreign surfaces. Never use it for a Nuah-owned surface: the host Mesa calls below are the correct ABI there. */
template <typename F> F atl_egl_function(const char* n) { return reinterpret_cast<F>(::dlsym(RTLD_DEFAULT, n)); }
struct EglPerfMetrics { std::mutex mutex; uint64_t count = 0, interval_total_ns = 0, interval_max_ns = 0, call_total_ns = 0, call_max_ns = 0, previous_ns = 0, next_report_ns = 0; };
EglPerfMetrics& egl_perf_metrics() { static EglPerfMetrics v; return v; }
// Shared env-gate helper: getenv cached in per-flag static, zero-alloc hot path.
#define NUAH_ENV_FLAG(fn, name) bool fn() { static const bool e = [] { const char* v = std::getenv(name); return v && *v && std::strcmp(v, "0") != 0; }(); return e; }
NUAH_ENV_FLAG(egl_perf_enabled, "NUAH_PERF_TRACE") NUAH_ENV_FLAG(egl_trace_enabled, "NUAH_EGL_TRACE")
uint64_t egl_monotonic_ns() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
void record_egl_swap(uint64_t started_ns, uint64_t finished_ns) {
  if (!egl_perf_enabled()) return;
  auto& m = egl_perf_metrics(); std::scoped_lock l(m.mutex); ++m.count;
  uint64_t call_ns = finished_ns - started_ns; m.call_total_ns += call_ns; m.call_max_ns = std::max(m.call_max_ns, call_ns);
  if (m.previous_ns != 0) { uint64_t iv = finished_ns - m.previous_ns; m.interval_total_ns += iv; m.interval_max_ns = std::max(m.interval_max_ns, iv); }
  m.previous_ns = finished_ns; if (m.next_report_ns == 0) m.next_report_ns = finished_ns + 1000000000ULL; if (finished_ns < m.next_report_ns) return;
  uint64_t ivs = m.count > 1 ? m.count - 1 : 0;
  std::fprintf(stderr, "nuah perf: egl swaps=%llu avg_interval_us=%llu max_interval_us=%llu avg_call_us=%llu max_call_us=%llu\n", static_cast<unsigned long long>(m.count), static_cast<unsigned long long>(ivs ? m.interval_total_ns / ivs / 1000ULL : 0), static_cast<unsigned long long>(m.interval_max_ns / 1000ULL), static_cast<unsigned long long>(m.call_total_ns / m.count / 1000ULL), static_cast<unsigned long long>(m.call_max_ns / 1000ULL));
  m.count = 0; m.interval_total_ns = 0; m.interval_max_ns = 0; m.call_total_ns = 0; m.call_max_ns = 0; m.previous_ns = 0; m.next_report_ns = finished_ns + 1000000000ULL; }
#define ATL_FALLBACK_SURFACE(ret, bname, d, s) if (!owns_surface(s)) { using A = ret (*)(EGLDisplay, EGLSurface); if (const auto a = atl_egl_function<A>(bname)) return a(d, s); }
#define ATL_FALLBACK_MAKECURRENT(d, dr, re, c) if (((dr) && !owns_surface(dr)) || ((re) && !owns_surface(re))) { using A = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext); if (const auto a = atl_egl_function<A>("bionic_eglMakeCurrent")) return a(d, dr, re, c); }
}  // namespace
extern "C" {
EGLDisplay bionic_eglGetDisplay(EGLNativeDisplayType) { NuahNativeWindow* w = nuah_native_window_default(); EGLDisplay d = display_for_window(w); if (w) nuah_native_window_release(w); if (d != EGL_NO_DISPLAY) return d; auto f = host_egl_function<EGLDisplay (*)(EGLNativeDisplayType)>("eglGetDisplay"); return f ? f(EGL_DEFAULT_DISPLAY) : EGL_NO_DISPLAY; }
#define NUAH_EGL_FORWARD(ret, bname, hname, proc, ...) { auto f = host_egl_function<proc>(hname); return f ? f(__VA_ARGS__) : ret; }
EGLBoolean bionic_eglChooseConfig(EGLDisplay d, EGLint* a, EGLConfig* c, EGLint n, EGLint* k) NUAH_EGL_FORWARD(EGL_FALSE, bionic_eglChooseConfig, "eglChooseConfig", PFNEGLCHOOSECONFIGPROC, d, a, c, n, k)
EGLSurface bionic_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint* a) NUAH_EGL_FORWARD(EGL_NO_SURFACE, bionic_eglCreatePbufferSurface, "eglCreatePbufferSurface", PFNEGLCREATEPBUFFERSURFACEPROC, d, c, a)
EGLSurface bionic_eglCreateWindowSurface(EGLDisplay d, EGLConfig c, NuahNativeWindow* w, const EGLint* a) {
  if (!w) return EGL_NO_SURFACE;
  auto f = host_egl_function<PFNEGLCREATEWINDOWSURFACEPROC>("eglCreateWindowSurface");
  EGLNativeWindowType n = reinterpret_cast<EGLNativeWindowType>(nuah_native_window_egl_handle(w)); if (!f || !n) return EGL_NO_SURFACE;
  EGLSurface s = f(d, c, n, a); if (s == EGL_NO_SURFACE) return s; nuah_native_window_acquire(w); std::scoped_lock l(surfaces_mutex); surfaces.emplace(s, w); return s; }
EGLBoolean bionic_eglDestroySurface(EGLDisplay d, EGLSurface s) { ATL_FALLBACK_SURFACE(EGLBoolean, "bionic_eglDestroySurface", d, s); auto f = host_egl_function<PFNEGLDESTROYSURFACEPROC>("eglDestroySurface"); EGLBoolean r = f ? f(d, s) : EGL_FALSE; NuahNativeWindow* w = nullptr; { std::scoped_lock l(surfaces_mutex); auto it = surfaces.find(s); if (it != surfaces.end()) { w = it->second; surfaces.erase(it); } } if (w) nuah_native_window_release(w); return r; }
EGLBoolean bionic_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface re, EGLContext c) { ATL_FALLBACK_MAKECURRENT(d, dr, re, c); auto f = host_egl_function<PFNEGLMAKECURRENTPROC>("eglMakeCurrent"); return f ? f(d, dr, re, c) : EGL_FALSE; }
EGLBoolean bionic_eglSwapBuffers(EGLDisplay d, EGLSurface s) { ATL_FALLBACK_SURFACE(EGLBoolean, "bionic_eglSwapBuffers", d, s); auto f = host_egl_function<PFNEGLSWAPBUFFERSPROC>("eglSwapBuffers"); static unsigned long calls = 0; uint64_t t0 = egl_perf_enabled() ? egl_monotonic_ns() : 0; EGLBoolean r = f ? f(d, s) : EGL_FALSE; if (egl_perf_enabled()) record_egl_swap(t0, egl_monotonic_ns()); if (egl_trace_enabled() && calls++ < 20) std::fprintf(stderr, "nuah egl: swap surface=%p result=%d\n", static_cast<void*>(s), r == EGL_TRUE ? 1 : 0); return r; }
EGLBoolean bionic_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint* v) {
  if (!owns_surface(s)) { using Q = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*); if (const auto q = atl_egl_function<Q>("bionic_eglQuerySurface")) { EGLBoolean r = q(d, s, a, v); if (r == EGL_TRUE || (a != EGL_WIDTH && a != EGL_HEIGHT)) return r; }
    /* Foreign surface may be an ATL object whose query impl is unavailable during early bootstrap. Active Nuah façade still has authoritative geometry; use it instead of passing an invalid ATLSurface pointer to Mesa. */
    if (v && (a == EGL_WIDTH || a == EGL_HEIGHT)) { NuahNativeWindow* w = nuah_native_window_default(); if (w) { int dim = a == EGL_WIDTH ? nuah_native_window_width(w) : nuah_native_window_height(w); nuah_native_window_release(w); if (dim > 0) { *v = dim; return EGL_TRUE; } } } }
  auto f = host_egl_function<PFNEGLQUERYSURFACEPROC>("eglQuerySurface"); EGLBoolean r = f ? f(d, s, a, v) : EGL_FALSE;
  if (egl_trace_enabled() && v && (a == EGL_WIDTH || a == EGL_HEIGHT)) std::fprintf(stderr, "nuah egl: query surface=%p attr=%s value=%d ok=%d\n", static_cast<void*>(s), a == EGL_WIDTH ? "width" : "height", *v, r == EGL_TRUE ? 1 : 0);
  return r; }
EGLSurface bionic_eglGetCurrentSurface(EGLint r) NUAH_EGL_FORWARD(EGL_NO_SURFACE, bionic_eglGetCurrentSurface, "eglGetCurrentSurface", PFNEGLGETCURRENTSURFACEPROC, r)
void (*bionic_eglGetProcAddress(const char* n))(void) { if (!n) return nullptr; using P = void (*)(void); static const struct { const char* n; P p; } k[] = {{"eglGetDisplay", reinterpret_cast<P>(&bionic_eglGetDisplay)}, {"eglCreateWindowSurface", reinterpret_cast<P>(&bionic_eglCreateWindowSurface)}, {"eglDestroySurface", reinterpret_cast<P>(&bionic_eglDestroySurface)}, {"eglMakeCurrent", reinterpret_cast<P>(&bionic_eglMakeCurrent)}, {"eglSwapBuffers", reinterpret_cast<P>(&bionic_eglSwapBuffers)}, {"eglQuerySurface", reinterpret_cast<P>(&bionic_eglQuerySurface)}}; for (const auto& e : k) if (std::strcmp(n, e.n) == 0) return e.p; using G = void (*(*)(const char*))(void); auto f = host_egl_function<G>("eglGetProcAddress"); return f ? f(n) : nullptr; }
EGLBoolean bionic_eglPresentationTimeANDROID(EGLDisplay, EGLSurface, EGLnsecsANDROID) { return EGL_TRUE; } EGLImage bionic_eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum t, EGLClientBuffer b, const EGLAttrib* a) NUAH_EGL_FORWARD(EGL_NO_IMAGE, bionic_eglCreateImageKHR, "eglCreateImage", PFNEGLCREATEIMAGEPROC, d, c, t, b, a)
EGLBoolean bionic_eglDestroyImageKHR(EGLDisplay d, EGLImage i) NUAH_EGL_FORWARD(EGL_FALSE, bionic_eglDestroyImageKHR, "eglDestroyImage", PFNEGLDESTROYIMAGEPROC, d, i) void bionic_glBindFramebuffer(GLenum t, GLuint f) { using B = void (*)(GLenum, GLuint); auto g = host_gles_function<B>("glBindFramebuffer"); if (g) g(t, f); }
}  // extern "C"
