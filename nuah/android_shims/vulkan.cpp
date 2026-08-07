#include "nuah/native_window_bridge.h"
#include "nuah/android_abi_registry.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
// vulkan_android.h requires Android NDK headers. This is the exact public ABI
// layout of VkAndroidSurfaceCreateInfoKHR, expressed using host-visible types.
struct AndroidSurfaceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  NuahNativeWindow* window;
};

struct SurfaceOwner {
  VkInstance instance = VK_NULL_HANDLE;
  NuahNativeWindow* window = nullptr;
};

std::mutex surfaces_mutex;
std::unordered_map<VkSurfaceKHR, SurfaceOwner> surfaces;

void* host_vulkan() {
  static void* library = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  return library;
}

template <typename Function>
Function host_function(const char* name) {
  void* library = host_vulkan();
  return library ? reinterpret_cast<Function>(::dlsym(library, name)) : nullptr;
}
}  // namespace

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
    VkInstance instance,
    const AndroidSurfaceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkSurfaceKHR* output) {
  nuah_android_api_register("libvulkan.so", "vkCreateAndroidSurfaceKHR",
                            NUAH_ANDROID_API_TRANSLATED);
  if (!instance || !create_info || !output ||
      create_info->sType != VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR ||
      !create_info->window) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  auto* host_window =
      static_cast<SDL_Window*>(nuah_native_window_host(create_info->window));
  if (!host_window) return VK_ERROR_INITIALIZATION_FAILED;

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  // Sober's native Android-surface shim unwraps its window and forwards a
  // VkWaylandSurfaceCreateInfoKHR to the host loader.  Prefer that exact
  // boundary when SDL exposes the underlying Wayland objects.  SDL remains a
  // compatibility fallback for X11 or a backend that does not publish them.
  const auto properties = SDL_GetWindowProperties(host_window);
  auto* display = static_cast<wl_display*>(SDL_GetPointerProperty(
      properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
  auto* wayland_surface = static_cast<wl_surface*>(SDL_GetPointerProperty(
      properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
  const auto create_wayland =
      host_function<PFN_vkCreateWaylandSurfaceKHR>("vkCreateWaylandSurfaceKHR");
  if (display && wayland_surface && create_wayland) {
    VkWaylandSurfaceCreateInfoKHR wayland_info{
        VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, nullptr, 0,
        display, wayland_surface};
    if (create_wayland(instance, &wayland_info, allocator, &surface) !=
        VK_SUCCESS) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  } else if (!SDL_Vulkan_CreateSurface(host_window, instance, allocator,
                                       &surface)) {
      return VK_ERROR_INITIALIZATION_FAILED;
  }
  nuah_native_window_acquire(create_info->window);
  bool inserted = false;
  {
    std::scoped_lock lock(surfaces_mutex);
    inserted = surfaces
                   .emplace(
                       surface,
                       SurfaceOwner{
                           .instance = instance,
                           .window = create_info->window,
                       })
                   .second;
  }
  if (!inserted) {
    SDL_Vulkan_DestroySurface(instance, surface, allocator);
    nuah_native_window_release(create_info->window);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  *output = surface;
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace) {
    std::fprintf(stderr,
                 "nuah vulkan: Android surface translated to host surface\n");
  }
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* allocator) {
  NuahNativeWindow* window = nullptr;
  {
    std::scoped_lock lock(surfaces_mutex);
    const auto found = surfaces.find(surface);
    if (found != surfaces.end()) {
      window = found->second.window;
      instance = found->second.instance;
      surfaces.erase(found);
    }
  }
  SDL_Vulkan_DestroySurface(instance, surface, allocator);
  nuah_native_window_release(window);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkInstance* output) {
  const auto function = host_function<PFN_vkCreateInstance>("vkCreateInstance");
  return function ? function(create_info, allocator, output)
                  : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* allocator) {
  std::vector<std::pair<VkSurfaceKHR, NuahNativeWindow*>> owned_surfaces;
  {
    std::scoped_lock lock(surfaces_mutex);
    for (auto iterator = surfaces.begin(); iterator != surfaces.end();) {
      if (iterator->second.instance == instance) {
        owned_surfaces.emplace_back(iterator->first, iterator->second.window);
        iterator = surfaces.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  for (const auto& [surface, window] : owned_surfaces) {
    SDL_Vulkan_DestroySurface(instance, surface, allocator);
    nuah_native_window_release(window);
  }
  const auto function = host_function<PFN_vkDestroyInstance>("vkDestroyInstance");
  if (function) function(instance, allocator);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* name) {
  const auto function =
      host_function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
  return function ? function(device, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* name) {
  nuah_android_api_register("libvulkan.so", "vkGetInstanceProcAddr",
                            NUAH_ANDROID_API_FORWARDED);
  if (!name) return nullptr;
  if (std::strcmp(name, "vkCreateAndroidSurfaceKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateAndroidSurfaceKHR);
  }
  if (std::strcmp(name, "vkDestroySurfaceKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroySurfaceKHR);
  }
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
  }
  const auto function =
      host_function<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
  return function ? function(instance, name) : nullptr;
}
}  // extern "C"
