#include "nuah/native_window_bridge.h"
#include <dlfcn.h>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <unordered_map>
struct NuahNativeWindow {
  /* Byte-compatible prefix with ATL's libandroid ANativeWindow (bionic_egl* take
   * this object from ANativeWindow_fromSurface). Direct EGL needs egl_window /
   * width / height; the rest stay null until a future adapter supplies them. */
  void* egl_window = nullptr;
  void* surface_view_widget = nullptr;
  void* wayland_display = nullptr;
  void* wayland_surface = nullptr;
  void* x11_display = nullptr;
  unsigned long resize_handler = 0;
  int refcount = 0x3fffffff;  // ATL may release its borrowed ABI view.
  int width = 0;
  int height = 0;
  /* Nuah owns the object independently of ATL's ABI-facing refcount. */
  std::atomic<unsigned> references{1};
  void* surface = nullptr;
  void* host_window = nullptr;
};
static_assert(offsetof(NuahNativeWindow, egl_window) == 0); static_assert(offsetof(NuahNativeWindow, surface_view_widget) == 8);
static_assert(offsetof(NuahNativeWindow, width) == 52); static_assert(offsetof(NuahNativeWindow, height) == 56);
namespace {
std::mutex registry_mutex;
std::unordered_map<void*, NuahNativeWindow*> registry;
std::unordered_map<void*, NuahNativeWindow*> egl_registry;
NuahNativeWindow* default_window = nullptr;
using WaylandEglWindowCreate = void* (*)(void*, int, int);
using WaylandEglWindowDestroy = void (*)(void*);
using WaylandEglWindowResize = void (*)(void*, int, int, int, int);
struct WaylandEglApi {
  void* library = nullptr;
  WaylandEglWindowCreate create = nullptr;
  WaylandEglWindowDestroy destroy = nullptr;
  WaylandEglWindowResize resize = nullptr;
};
struct WaylandEglState {
  WaylandEglApi api;
  std::atomic<unsigned> state{0};  // 0=unloaded, 1=loading, 2=finished
};
WaylandEglState wayland_egl_state;
WaylandEglApi empty_wayland_egl_api;
WaylandEglApi& wayland_egl_api() {
  if (wayland_egl_state.state.load(std::memory_order_acquire) == 2) return wayland_egl_state.api;
  unsigned expected = 0;
  if (!wayland_egl_state.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return empty_wayland_egl_api;
  WaylandEglApi result;
  result.library = ::dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
  if (result.library) {
    static const char* kSyms[] = {"wl_egl_window_create", "wl_egl_window_destroy", "wl_egl_window_resize"};
    void* sym[3] = {::dlsym(result.library, kSyms[0]), ::dlsym(result.library, kSyms[1]), ::dlsym(result.library, kSyms[2])};
    result.create = reinterpret_cast<WaylandEglWindowCreate>(sym[0]); result.destroy = reinterpret_cast<WaylandEglWindowDestroy>(sym[1]); result.resize = reinterpret_cast<WaylandEglWindowResize>(sym[2]);
    if (!result.create || !result.destroy) { ::dlclose(result.library); result = {}; }
  }
  wayland_egl_state.api = result;
  wayland_egl_state.state.store(2, std::memory_order_release);
  return wayland_egl_state.api;
}
NuahNativeWindow* resolve_window_locked(NuahNativeWindow* value) {
  if (!value) return nullptr;
  const auto found = egl_registry.find(value);
  return found == egl_registry.end() ? value : found->second;
}
NuahNativeWindow* acquire_locked(NuahNativeWindow* value) {
  // Caller holds registry_mutex; increment directly (public acquire would deadlock).
  auto* window = resolve_window_locked(value);
  if (window) window->references.fetch_add(1, std::memory_order_relaxed);
  return window;
}
// Shared with-lock resolve: keeps every trivial getter on one line.
template <typename F> auto with_resolved(NuahNativeWindow* window, F&& fn) -> decltype(fn(window)) {
  std::scoped_lock lock(registry_mutex);
  return fn(resolve_window_locked(window));
}
void destroy_egl_window_handle(void* egl_window) {
  if (!egl_window) return;
  auto& api = wayland_egl_api();
  if (api.destroy) api.destroy(egl_window);
}
}  // namespace
extern "C" NuahNativeWindow* nuah_native_window_register_surface(void* surface, void* host_window, int width, int height) {
  if (!surface || !host_window || width <= 0 || height <= 0) return nullptr;
  auto* window = new NuahNativeWindow;
  window->surface = surface; window->host_window = host_window; window->width = width; window->height = height;
  std::scoped_lock lock(registry_mutex);
  if (registry.contains(surface)) { delete window; return nullptr; }
  registry.emplace(surface, window);
  return window;
}
extern "C" void nuah_native_window_unregister_surface(void* surface) {
  NuahNativeWindow* window = nullptr;
  {
    std::scoped_lock lock(registry_mutex);
    const auto found = registry.find(surface);
    if (found == registry.end()) return;
    window = found->second;
    registry.erase(found);
    if (default_window == window) default_window = nullptr;
  }
  nuah_native_window_release(window);
}
extern "C" int nuah_native_window_alias_surface(NuahNativeWindow* window, void* surface) {
  if (!window || !surface) return 0;
  std::scoped_lock lock(registry_mutex);
  if (registry.contains(surface)) return 0;
  auto* retained = acquire_locked(window);
  if (!retained) return 0;
  registry.emplace(surface, retained);
  return 1;
}
extern "C" NuahNativeWindow* nuah_native_window_from_surface(void* surface) {
  std::scoped_lock lock(registry_mutex);
  const auto found = registry.find(surface);
  return found == registry.end() ? nullptr : acquire_locked(found->second);
}
extern "C" NuahNativeWindow* nuah_native_window_default(void) {
  std::scoped_lock lock(registry_mutex);
  return default_window ? acquire_locked(default_window) : nullptr;
}
extern "C" void nuah_native_window_set_default(NuahNativeWindow* window) {
  std::scoped_lock lock(registry_mutex);
  default_window = resolve_window_locked(window);
}
extern "C" void nuah_native_window_acquire(NuahNativeWindow* window) { with_resolved(window, [](NuahNativeWindow* r) { if (r) r->references.fetch_add(1, std::memory_order_relaxed); }); }
extern "C" void nuah_native_window_release(NuahNativeWindow* window) {
  void* egl_window = nullptr;
  bool delete_window = false;
  {
    std::scoped_lock lock(registry_mutex);
    window = resolve_window_locked(window);
    if (window && window->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      egl_window = window->egl_window;
      if (egl_window) egl_registry.erase(egl_window);
      window->egl_window = nullptr;
      delete_window = true;
    }
  }
  /* Never call Wayland while holding the mutex: compositor callbacks can re-enter bookkeeping (registry -> Wayland -> registry cycle). */
  if (delete_window) { destroy_egl_window_handle(egl_window); delete window; }
}
extern "C" int nuah_native_window_width(const NuahNativeWindow* window) { return with_resolved(const_cast<NuahNativeWindow*>(window), [](NuahNativeWindow* r) { return r ? r->width : 0; }); }
extern "C" int nuah_native_window_height(const NuahNativeWindow* window) { return with_resolved(const_cast<NuahNativeWindow*>(window), [](NuahNativeWindow* r) { return r ? r->height : 0; }); }
extern "C" void* nuah_native_window_host(const NuahNativeWindow* window) { return with_resolved(const_cast<NuahNativeWindow*>(window), [](NuahNativeWindow* r) -> void* { return r ? r->host_window : nullptr; }); }
extern "C" void* nuah_native_window_egl_handle(const NuahNativeWindow* window) { return with_resolved(const_cast<NuahNativeWindow*>(window), [](NuahNativeWindow* r) -> void* { return r && r->egl_window ? r->egl_window : const_cast<NuahNativeWindow*>(r); }); }
extern "C" void nuah_native_window_set_egl_handle(NuahNativeWindow* window, void* egl_window) {
  if (!window || !egl_window) return;
  std::scoped_lock lock(registry_mutex);
  window = resolve_window_locked(window);
  if (!window || window->egl_window) return;
  window->egl_window = egl_window;
  egl_registry.emplace(egl_window, window);
}
extern "C" void nuah_native_window_update_geometry(NuahNativeWindow* window, int width, int height) {
  if (width <= 0 || height <= 0) return;
  void* egl_window = nullptr;
  NuahNativeWindow* retained = nullptr;
  {
    std::scoped_lock lock(registry_mutex);
    window = resolve_window_locked(window);
    if (!window) return;
    const bool changed = window->width != width || window->height != height;
    window->width = width;
    window->height = height;
    /* Host pump runs at 100 Hz: skip wl_egl_window_resize for unchanged size (avoids compositor traffic / surface rebuilds in lobby). */
    if (changed && window->egl_window) {
      /* Keep the façade alive while the external Wayland call runs. */
      window->references.fetch_add(1, std::memory_order_relaxed);
      retained = window;
      egl_window = window->egl_window;
    }
  }
  if (egl_window) {
    auto& wayland = wayland_egl_api();
    if (wayland.resize) wayland.resize(egl_window, width, height, 0, 0);
  }
  if (retained) nuah_native_window_release(retained);
}
