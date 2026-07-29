#include "nuah/native_window_bridge.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

struct NuahNativeWindow {
  std::atomic<unsigned> references{1};
  void* surface = nullptr;
  void* host_window = nullptr;
  std::atomic<int> width{0};
  std::atomic<int> height{0};
};

namespace {
std::mutex registry_mutex;
std::unordered_map<void*, NuahNativeWindow*> registry;
}  // namespace

extern "C" NuahNativeWindow* nuah_native_window_register_surface(
    void* surface,
    void* host_window,
    int width,
    int height) {
  if (!surface || !host_window || width <= 0 || height <= 0) return nullptr;
  auto* window = new NuahNativeWindow;
  window->surface = surface;
  window->host_window = host_window;
  window->width.store(width, std::memory_order_relaxed);
  window->height.store(height, std::memory_order_relaxed);

  std::scoped_lock lock(registry_mutex);
  if (registry.contains(surface)) {
    delete window;
    return nullptr;
  }
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
  }
  nuah_native_window_release(window);
}

extern "C" int nuah_native_window_alias_surface(NuahNativeWindow* window,
                                                  void* surface) {
  if (!window || !surface) return 0;
  std::scoped_lock lock(registry_mutex);
  if (registry.contains(surface)) return 0;
  nuah_native_window_acquire(window);
  registry.emplace(surface, window);
  return 1;
}

extern "C" NuahNativeWindow* nuah_native_window_from_surface(void* surface) {
  std::scoped_lock lock(registry_mutex);
  const auto found = registry.find(surface);
  if (found == registry.end()) return nullptr;
  nuah_native_window_acquire(found->second);
  return found->second;
}

extern "C" void nuah_native_window_acquire(NuahNativeWindow* window) {
  if (window) window->references.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void nuah_native_window_release(NuahNativeWindow* window) {
  if (window &&
      window->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete window;
  }
}

extern "C" int nuah_native_window_width(const NuahNativeWindow* window) {
  return window ? window->width.load(std::memory_order_relaxed) : 0;
}

extern "C" int nuah_native_window_height(const NuahNativeWindow* window) {
  return window ? window->height.load(std::memory_order_relaxed) : 0;
}

extern "C" void* nuah_native_window_host(const NuahNativeWindow* window) {
  return window ? window->host_window : nullptr;
}

extern "C" void nuah_native_window_update_geometry(
    NuahNativeWindow* window,
    int width,
    int height) {
  if (!window || width <= 0 || height <= 0) return;
  window->width.store(width, std::memory_order_relaxed);
  window->height.store(height, std::memory_order_relaxed);
}
