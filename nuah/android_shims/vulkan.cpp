#include "nuah/native_window_bridge.h"
#include "nuah/android_abi_registry.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
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

enum class RequestedPresentMode { none, fifo, mailbox, immediate };

RequestedPresentMode requested_present_mode() {
  static const RequestedPresentMode value = [] {
    const char* raw = std::getenv("NUAH_VULKAN_PRESENT_MODE");
    if (!raw) return RequestedPresentMode::none;
    if (std::strcmp(raw, "fifo") == 0) return RequestedPresentMode::fifo;
    if (std::strcmp(raw, "mailbox") == 0) return RequestedPresentMode::mailbox;
    if (std::strcmp(raw, "immediate") == 0)
      return RequestedPresentMode::immediate;
    return RequestedPresentMode::none;
  }();
  return value;
}

VkPresentModeKHR requested_present_mode_value() {
  switch (requested_present_mode()) {
    case RequestedPresentMode::fifo: return VK_PRESENT_MODE_FIFO_KHR;
    case RequestedPresentMode::mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
    case RequestedPresentMode::immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case RequestedPresentMode::none: break;
  }
  return VK_PRESENT_MODE_MAX_ENUM_KHR;
}

bool perf_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_PERF_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

uint64_t monotonic_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct PresentMetrics {
  std::mutex mutex;
  uint64_t count = 0;
  uint64_t interval_total_ns = 0;
  uint64_t interval_max_ns = 0;
  uint64_t call_total_ns = 0;
  uint64_t call_max_ns = 0;
  uint64_t previous_ns = 0;
  uint64_t next_report_ns = 0;
};

PresentMetrics& present_metrics() {
  static PresentMetrics value;
  return value;
}

void record_present(uint64_t started_ns, uint64_t finished_ns) {
  PresentMetrics& metrics = present_metrics();
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
  if (metrics.next_report_ns == 0) metrics.next_report_ns = finished_ns + 1000000000ULL;
  if (finished_ns < metrics.next_report_ns) return;
  const uint64_t intervals = metrics.count > 1 ? metrics.count - 1 : 0;
  std::fprintf(stderr,
               "nuah perf: vulkan presents=%llu avg_interval_us=%llu max_interval_us=%llu avg_call_us=%llu max_call_us=%llu\n",
               static_cast<unsigned long long>(metrics.count),
               static_cast<unsigned long long>(
                   intervals ? metrics.interval_total_ns / intervals / 1000ULL
                              : 0),
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

void trace_present_mode(const char* mode, bool available) {
  const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
  if (!trace || !*trace) return;
  std::fprintf(stderr, "nuah vulkan: requested present mode=%s %s\n", mode,
               available ? "enabled" : "unavailable; keeping host modes");
}

NuahNativeWindow* retain_window_for_surface(VkSurfaceKHR surface) {
  std::scoped_lock lock(surfaces_mutex);
  const auto found = surfaces.find(surface);
  if (found == surfaces.end() || !found->second.window) return nullptr;
  nuah_native_window_acquire(found->second.window);
  return found->second.window;
}

void normalize_surface_extent(VkSurfaceCapabilitiesKHR* capabilities,
                              NuahNativeWindow* window) {
  if (!capabilities || !window) return;
  auto* host_window =
      static_cast<SDL_Window*>(nuah_native_window_host(window));
  if (!host_window) return;
  int width = 0;
  int height = 0;
  if (SDL_GetWindowSize(host_window, &width, &height) <= 0 || width <= 0 ||
      height <= 0)
    return;
  const auto clamp_extent = [](uint32_t value, uint32_t minimum,
                               uint32_t maximum) {
    return std::min(std::max(value, minimum), maximum);
  };
  /* Wayland uses UINT32_MAX to mean “choose the extent”; Android's WSI
   * exposes the realized ANativeWindow size instead.  Roblox expects the
   * latter and otherwise loops reporting `currentExtent -1x-1`. */
  if (capabilities->currentExtent.width == UINT32_MAX ||
      capabilities->currentExtent.height == UINT32_MAX ||
      capabilities->currentExtent.width == 0 ||
      capabilities->currentExtent.height == 0) {
    capabilities->currentExtent.width = clamp_extent(
        static_cast<uint32_t>(width), capabilities->minImageExtent.width,
        capabilities->maxImageExtent.width);
    capabilities->currentExtent.height = clamp_extent(
        static_cast<uint32_t>(height), capabilities->minImageExtent.height,
        capabilities->maxImageExtent.height);
  }
}

void* host_vulkan() {
  static void* library = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  return library;
}

template <typename Function>
Function host_function(const char* name) {
  void* library = host_vulkan();
  return library ? reinterpret_cast<Function>(::dlsym(library, name)) : nullptr;
}

bool host_has_instance_extension(const char* wanted) {
  if (!wanted) return false;
  const auto enumerate =
      host_function<PFN_vkEnumerateInstanceExtensionProperties>(
          "vkEnumerateInstanceExtensionProperties");
  if (!enumerate) return false;
  uint32_t count = 0;
  if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
    return false;
  std::vector<VkExtensionProperties> extensions(count);
  if (enumerate(nullptr, &count, extensions.data()) != VK_SUCCESS)
    return false;
  for (const auto& extension : extensions) {
    if (std::strcmp(extension.extensionName, wanted) == 0) return true;
  }
  return false;
}
}  // namespace

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* present_info);

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
  bool inserted = false;
  {
    /* Keep the lock order identical to retain_window_for_surface(): the
     * Vulkan surface table is outermost, then the native-window registry.
     * Acquiring the window before this block inverted that order and allowed
     * a concurrent surface-capabilities query to deadlock with creation. */
    std::scoped_lock lock(surfaces_mutex);
    nuah_native_window_acquire(create_info->window);
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

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* capabilities) {
  const auto function =
      host_function<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  if (!function || !capabilities) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = function(physical_device, surface, capabilities);
  if (result != VK_SUCCESS) return result;
  NuahNativeWindow* window = retain_window_for_surface(surface);
  normalize_surface_extent(capabilities, window);
  if (window) nuah_native_window_release(window);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceSurfaceInfo2KHR* surface_info,
    VkSurfaceCapabilities2KHR* capabilities) {
  const auto function =
      host_function<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
          "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
  if (!function || !surface_info || !capabilities)
    return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result =
      function(physical_device, surface_info, capabilities);
  if (result != VK_SUCCESS) return result;
  NuahNativeWindow* window = retain_window_for_surface(surface_info->surface);
  normalize_surface_extent(&capabilities->surfaceCapabilities, window);
  if (window) nuah_native_window_release(window);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    uint32_t* count, VkPresentModeKHR* modes) {
  const auto function =
      host_function<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
          "vkGetPhysicalDeviceSurfacePresentModesKHR");
  if (!function || !count) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = function(physical_device, surface, count, modes);
  if (result != VK_SUCCESS || !modes || *count == 0 ||
      requested_present_mode() == RequestedPresentMode::none)
    return result;

  const VkPresentModeKHR wanted = requested_present_mode_value();
  uint32_t wanted_index = *count;
  for (uint32_t index = 0; index < *count; ++index) {
    if (modes[index] == wanted) {
      wanted_index = index;
      break;
    }
  }
  const char* mode_name =
      requested_present_mode() == RequestedPresentMode::fifo
          ? "fifo"
          : requested_present_mode() == RequestedPresentMode::mailbox
                ? "mailbox"
                : "immediate";
  if (wanted_index == *count) {
    trace_present_mode(mode_name, false);
    return result;
  }
  /* Expose only the requested advertised mode.  Merely moving FIFO to the
   * front is insufficient for clients that explicitly prefer IMMEDIATE;
   * filtering makes this A/B knob deterministic without inventing a driver
   * capability. */
  modes[0] = modes[wanted_index];
  *count = 1;
  trace_present_mode(mode_name, true);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkInstance* output) {
  const auto function = host_function<PFN_vkCreateInstance>("vkCreateInstance");
  if (!function || !create_info || !output)
    return VK_ERROR_INITIALIZATION_FAILED;

  /* Roblox asks for the Android WSI extension because it is running against
   * the Android ABI.  A Linux Vulkan loader quite correctly rejects that
   * extension, even though Nuah translates the later Android-surface call to
   * Wayland.  Keep every other application extension untouched, replace only
   * this WSI name with the host WSI extension, and let the existing
   * vkCreateAndroidSurfaceKHR adapter perform the actual window translation. */
  bool requested_android_surface = false;
  bool has_wayland_surface = false;
  std::vector<const char*> extensions;
  extensions.reserve(create_info->enabledExtensionCount + 1);
  for (uint32_t index = 0; index < create_info->enabledExtensionCount; ++index) {
    const char* name = create_info->ppEnabledExtensionNames[index];
    if (!name) continue;
    if (std::strcmp(name, "VK_KHR_android_surface") == 0) {
      requested_android_surface = true;
      continue;
    }
    if (std::strcmp(name, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0)
      has_wayland_surface = true;
    extensions.push_back(name);
  }
  if (requested_android_surface && !has_wayland_surface &&
      host_has_instance_extension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
    has_wayland_surface = true;
  }

  VkInstanceCreateInfo forwarded = *create_info;
  if (requested_android_surface) {
    forwarded.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    forwarded.ppEnabledExtensionNames = extensions.data();
    if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::fprintf(stderr,
                   "nuah vulkan: replaced VK_KHR_android_surface with %s\n",
                   has_wayland_surface ? VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
                                       : "no host WSI extension");
    }
  }
  return function(&forwarded, allocator, output);
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
  if (name && std::strcmp(name, "vkQueuePresentKHR") == 0 &&
      perf_trace_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHR);
  }
  if (name &&
      std::strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(
        &vkGetPhysicalDeviceSurfacePresentModesKHR);
  }
  return function ? function(device, name) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* present_info) {
  static const auto function = host_function<PFN_vkQueuePresentKHR>(
      "vkQueuePresentKHR");
  if (!function) return VK_ERROR_DEVICE_LOST;
  if (!perf_trace_enabled()) return function(queue, present_info);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue, present_info);
  record_present(started_ns, monotonic_ns());
  return result;
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
  if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(
        &vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
  }
  if (std::strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(
        &vkGetPhysicalDeviceSurfaceCapabilities2KHR);
  }
  if (std::strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(
        &vkGetPhysicalDeviceSurfacePresentModesKHR);
  }
  if (std::strcmp(name, "vkQueuePresentKHR") == 0 && perf_trace_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHR);
  }
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
  }
  const auto function =
      host_function<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
  return function ? function(instance, name) : nullptr;
}
}  // extern "C"
