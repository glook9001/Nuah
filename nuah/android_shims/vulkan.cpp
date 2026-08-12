#include "nuah/native_window_bridge.h"
#include "nuah/android_abi_registry.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

enum class RequestedPresentMode {
  none,
  fifo,
  fifo_relaxed,
  mailbox,
  immediate,
};

RequestedPresentMode requested_present_mode() {
  static const RequestedPresentMode value = [] {
    const char* raw = std::getenv("NUAH_VULKAN_PRESENT_MODE");
    /* The Android client expects a compositor-paced swap loop.  On this
     * Wayland/Intel path FIFO gives a stable ~16.6 ms cadence in the same
     * Roblox room, while immediate mode produces uneven 22--35 ms intervals
     * and feels like a slideshow.  Keep immediate available as an explicit
     * low-latency diagnostic override. */
    if (!raw || !*raw) return RequestedPresentMode::fifo;
    if (std::strcmp(raw, "fifo") == 0) return RequestedPresentMode::fifo;
    if (std::strcmp(raw, "fifo_relaxed") == 0)
      return RequestedPresentMode::fifo_relaxed;
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
    case RequestedPresentMode::fifo_relaxed:
      return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
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
  PresentMetrics() { interval_samples.reserve(240); }

  std::mutex mutex;
  uint64_t count = 0;
  uint64_t interval_total_ns = 0;
  uint64_t interval_max_ns = 0;
  uint64_t call_total_ns = 0;
  uint64_t call_max_ns = 0;
  uint64_t previous_ns = 0;
  uint64_t window_started_ns = 0;
  uint64_t next_report_ns = 0;
  std::vector<uint64_t> interval_samples;
};

struct PresentReport {
  bool ready = false;
  uint64_t count = 0;
  uint64_t interval_count = 0;
  uint64_t interval_total_ns = 0;
  uint64_t interval_max_ns = 0;
  uint64_t call_total_ns = 0;
  uint64_t call_max_ns = 0;
  uint64_t window_ns = 0;
  std::vector<uint64_t> interval_samples;
};

PresentMetrics& present_metrics() {
  static PresentMetrics value;
  return value;
}

uint64_t percentile(const std::vector<uint64_t>& sorted,
                    uint64_t percent) {
  if (sorted.empty()) return 0;
  const std::size_t rank = static_cast<std::size_t>(
      (sorted.size() * percent + 99ULL) / 100ULL);
  return sorted[std::min(std::max<std::size_t>(rank, 1), sorted.size()) - 1];
}

void record_present(uint64_t started_ns, uint64_t finished_ns) {
  PresentMetrics& metrics = present_metrics();
  PresentReport report;
  {
    std::scoped_lock lock(metrics.mutex);
    ++metrics.count;
    const uint64_t call_ns = finished_ns - started_ns;
    metrics.call_total_ns += call_ns;
    metrics.call_max_ns = std::max(metrics.call_max_ns, call_ns);
    if (metrics.previous_ns != 0) {
      const uint64_t interval_ns = finished_ns - metrics.previous_ns;
      metrics.interval_total_ns += interval_ns;
      metrics.interval_max_ns = std::max(metrics.interval_max_ns, interval_ns);
      metrics.interval_samples.push_back(interval_ns);
    }
    metrics.previous_ns = finished_ns;
    if (metrics.next_report_ns == 0) {
      metrics.window_started_ns = finished_ns;
      metrics.next_report_ns = finished_ns + 1000000000ULL;
    }
    if (finished_ns < metrics.next_report_ns) return;
    report.ready = true;
    report.count = metrics.count;
    report.interval_count = metrics.interval_samples.size();
    report.interval_total_ns = metrics.interval_total_ns;
    report.interval_max_ns = metrics.interval_max_ns;
    report.call_total_ns = metrics.call_total_ns;
    report.call_max_ns = metrics.call_max_ns;
    report.window_ns = finished_ns - metrics.window_started_ns;
    report.interval_samples.swap(metrics.interval_samples);
    metrics.count = 0;
    metrics.interval_total_ns = 0;
    metrics.interval_max_ns = 0;
    metrics.call_total_ns = 0;
    metrics.call_max_ns = 0;
    metrics.window_started_ns = finished_ns;
    metrics.next_report_ns = finished_ns + 1000000000ULL;
  }
  if (!report.ready) return;
  std::sort(report.interval_samples.begin(), report.interval_samples.end());
  const uint64_t p50_ns = percentile(report.interval_samples, 50);
  const uint64_t p95_ns = percentile(report.interval_samples, 95);
  const uint64_t p99_ns = percentile(report.interval_samples, 99);
  const auto count_over = [&](uint64_t threshold_ns) {
    return static_cast<uint64_t>(report.interval_samples.end() -
        std::upper_bound(report.interval_samples.begin(),
                         report.interval_samples.end(), threshold_ns));
  };
  const uint64_t fps_milli = report.window_ns
      ? report.count * 1000000000000ULL / report.window_ns
      : 0;
  std::fprintf(stderr,
               "nuah perf: vulkan presents=%llu fps=%llu.%03llu avg_interval_us=%llu p50_us=%llu p95_us=%llu p99_us=%llu max_interval_us=%llu over33ms=%llu over50ms=%llu over100ms=%llu avg_call_us=%llu max_call_us=%llu\n",
               static_cast<unsigned long long>(report.count),
               static_cast<unsigned long long>(fps_milli / 1000ULL),
               static_cast<unsigned long long>(fps_milli % 1000ULL),
               static_cast<unsigned long long>(
                   report.interval_count ? report.interval_total_ns /
                                          report.interval_count / 1000ULL
                              : 0),
               static_cast<unsigned long long>(p50_ns / 1000ULL),
               static_cast<unsigned long long>(p95_ns / 1000ULL),
               static_cast<unsigned long long>(p99_ns / 1000ULL),
               static_cast<unsigned long long>(report.interval_max_ns / 1000ULL),
               static_cast<unsigned long long>(count_over(33333333ULL)),
               static_cast<unsigned long long>(count_over(50000000ULL)),
               static_cast<unsigned long long>(count_over(100000000ULL)),
               static_cast<unsigned long long>(report.call_total_ns /
                                               report.count / 1000ULL),
               static_cast<unsigned long long>(report.call_max_ns / 1000ULL));
}

void trace_present_mode(const char* mode, bool available) {
  const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
  if (!trace || !*trace) return;
  std::fprintf(stderr, "nuah vulkan: requested present mode=%s %s\n", mode,
               available ? "enabled" : "unavailable; keeping host modes");
}

bool surface_size_locked() {
  const char* raw = std::getenv("NUAH_LOCK_SURFACE_SIZE");
  return raw && *raw && std::strcmp(raw, "0") != 0;
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
  int width = nuah_native_window_width(window);
  int height = nuah_native_window_height(window);
  if ((width <= 0 || height <= 0) && host_window) {
    if (SDL_GetWindowSize(host_window, &width, &height) <= 0 || width <= 0 ||
        height <= 0)
      return;
  }
  const auto clamp_extent = [](uint32_t value, uint32_t minimum,
                               uint32_t maximum) {
    return std::min(std::max(value, minimum), maximum);
  };
  /* Wayland uses UINT32_MAX to mean “choose the extent”; Android's WSI
   * exposes the realized ANativeWindow size instead.  Roblox expects the
   * latter and otherwise loops reporting `currentExtent -1x-1`. */
  if (surface_size_locked() ||
      capabilities->currentExtent.width == UINT32_MAX ||
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

/* Sober's Android WSI commonly runs with four swapchain images.  Keep the
 * host driver's advertised range authoritative, use that depth for the
 * measured turbo profile, and allow an explicit A/B override when the
 * minimum is lower and the driver permits the deeper queue.  Balanced and
 * quality launches retain the driver's native depth because image count
 * changes pacing and latency. */
void normalize_swapchain_depth(VkSurfaceCapabilitiesKHR* capabilities) {
  if (!capabilities) return;
  const char* raw = std::getenv("NUAH_VULKAN_MIN_IMAGE_COUNT");
  unsigned long wanted = 0;
  if (raw && *raw) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed < 1 || parsed > UINT32_MAX)
      return;
    wanted = parsed;
  } else {
    /* Sober's Android Vulkan path requests four swapchain images.  On the
     * Intel/Wayland path this leaves one image available while the render
     * thread is waiting for FIFO, preventing a compositor starvation burst
     * during ordinary scene streaming.  Keep the deeper queue limited to the
     * measured turbo profile; balanced/quality launches retain the driver's
     * native depth.  Set NUAH_VULKAN_MIN_IMAGE_COUNT=3 to restore the old
     * three-image negotiation for a direct A/B comparison. */
    const char* performance = std::getenv("NUAH_PERFORMANCE_MODE");
    if (!performance ||
        (std::strcmp(performance, "turbo") != 0 &&
         std::strcmp(performance, "fast") != 0))
      return;
    wanted = 4;
  }
  const uint32_t requested = static_cast<uint32_t>(wanted);
  if (requested <= capabilities->minImageCount ||
      (capabilities->maxImageCount != 0 &&
       requested > capabilities->maxImageCount))
    return;
  capabilities->minImageCount = requested;
}

struct LazyLibrary {
  std::atomic<void*> handle{nullptr};
  std::atomic<unsigned> state{0};  // 0=unloaded, 1=loading, 2=finished
};

void* host_vulkan() {
  static LazyLibrary library;
  if (void* handle = library.handle.load(std::memory_order_acquire))
    return handle;
  unsigned expected = 0;
  if (!library.state.compare_exchange_strong(expected, 1,
                                             std::memory_order_acq_rel)) {
    /* A constructor can re-enter the provider while dlopen is resolving it.
     * Returning nullptr is a bounded failure; waiting on a C++ guard here
     * would turn that re-entry into a process-wide deadlock. */
    return nullptr;
  }
  void* handle = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  library.handle.store(handle, std::memory_order_release);
  library.state.store(2, std::memory_order_release);
  return handle;
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

bool pipeline_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VULKAN_PIPELINE_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool pipeline_callsite_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VULKAN_CALLSITE_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool wait_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VULKAN_WAIT_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool submit_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VULKAN_SUBMIT_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool copy_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VULKAN_COPY_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool residency_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VK_RESIDENCY");
    if (!raw || !*raw) return true;
    return std::strcmp(raw, "0") != 0 &&
           std::strcmp(raw, "off") != 0 &&
           std::strcmp(raw, "false") != 0 &&
           std::strcmp(raw, "no") != 0;
  }();
  return value;
}

bool memory_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VULKAN_MEMORY_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

void trace_memory_properties(VkPhysicalDevice physical_device,
                             const VkPhysicalDeviceMemoryProperties& memory) {
  if (!memory_trace_enabled()) return;
  static std::mutex trace_mutex;
  static std::unordered_set<VkPhysicalDevice> traced;
  std::scoped_lock lock(trace_mutex);
  if (!traced.insert(physical_device).second) return;

  uint32_t host_visible_types = 0;
  for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
    if (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      ++host_visible_types;
  }
  std::fprintf(stderr,
               "nuah vulkan memory: heaps=%u types=%u host_visible_types=%u\n",
               memory.memoryHeapCount, memory.memoryTypeCount,
               host_visible_types);
  for (uint32_t index = 0; index < memory.memoryHeapCount; ++index) {
    std::fprintf(stderr,
                 "nuah vulkan memory: heap[%u] size=%llu flags=0x%x\n", index,
                 static_cast<unsigned long long>(memory.memoryHeaps[index].size),
                 memory.memoryHeaps[index].flags);
  }
}

bool residency_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VK_RESIDENCY_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

/* Snapshotting a live caller-owned cache is useful for a controlled warm-up
 * run, but it asks the driver to serialize the cache on the render thread.
 * Keep it opt-in; normal runs persist at vkDeviceWaitIdle/vkDestroyDevice. */
bool residency_snapshot_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VK_RESIDENCY_SNAPSHOT");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

std::filesystem::path pipeline_cache_path() {
  const char* directory = std::getenv("NUAH_SHADER_CACHE_DIR");
  /* configure_mesa_shader_cache publishes the profile location through
   * MESA_SHADER_CACHE_DIR for ordinary launches.  Falling back to it keeps
   * the Vulkan pipeline cache beside Mesa's cache instead of silently
   * disabling persistence unless callers duplicate that environment knob. */
  if (!directory || !*directory)
    directory = std::getenv("MESA_SHADER_CACHE_DIR");
  if (!directory || !*directory) return {};
  return std::filesystem::path(directory) / "nuah-vk-pipeline-cache.bin";
}

std::mutex pipeline_cache_file_mutex;
std::mutex persistent_pipeline_cache_mutex;
std::unordered_map<VkDevice, VkPipelineCache> persistent_pipeline_caches;
/* Roblox normally creates and owns the cache handle itself.  Keep a weak
 * bookkeeping set for those handles so the cache can be serialized at an
 * idle/teardown boundary without replacing or extending Roblox's lifetime. */
std::unordered_map<VkDevice, std::unordered_set<VkPipelineCache>>
    caller_pipeline_caches;

std::vector<uint8_t> load_pipeline_cache_data() {
  if (!residency_enabled()) return {};
  const std::filesystem::path path = pipeline_cache_path();
  if (path.empty()) return {};
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return {};
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return {};
  const std::streamsize size = input.tellg();
  if (size <= 0 || size > 256 * 1024 * 1024) return {};
  std::vector<uint8_t> data(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(data.data()), size)) return {};
  return data;
}

void save_pipeline_cache_data(const void* raw_data, std::size_t size) {
  if (!residency_enabled() || !raw_data || size == 0) return;
  const std::filesystem::path path = pipeline_cache_path();
  if (path.empty() || size > 256 * 1024 * 1024) return;
  std::scoped_lock lock(pipeline_cache_file_mutex);
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return;
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output.write(static_cast<const char*>(raw_data),
                 static_cast<std::streamsize>(size));
    if (!output) return;
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
  }
}

VkPipelineCache persistent_pipeline_cache(VkDevice device) {
  if (!device || !residency_enabled()) return VK_NULL_HANDLE;
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  const auto found = persistent_pipeline_caches.find(device);
  if (found != persistent_pipeline_caches.end()) return found->second;
  const auto function =
      host_function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache");
  if (!function) return VK_NULL_HANDLE;
  VkPipelineCacheCreateInfo create_info{
      VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
  std::vector<uint8_t> disk_data = load_pipeline_cache_data();
  if (!disk_data.empty()) {
    create_info.initialDataSize = disk_data.size();
    create_info.pInitialData = disk_data.data();
  }
  VkPipelineCache cache = VK_NULL_HANDLE;
  VkResult result = function(device, &create_info, nullptr, &cache);
  if (result != VK_SUCCESS && !disk_data.empty()) {
    create_info.initialDataSize = 0;
    create_info.pInitialData = nullptr;
    result = function(device, &create_info, nullptr, &cache);
  }
  if (result != VK_SUCCESS || cache == VK_NULL_HANDLE) return VK_NULL_HANDLE;
  if (residency_trace_enabled())
    std::fprintf(stderr,
                 "nuah residency: created persistent pipeline cache seed_bytes=%zu path=%s\n",
                 disk_data.size(), pipeline_cache_path().c_str());
  persistent_pipeline_caches.emplace(device, cache);
  return cache;
}

void save_pipeline_cache_handle(VkDevice device, VkPipelineCache cache) {
  if (!device || !cache || !residency_enabled()) return;
  const auto function =
      host_function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
  if (!function) return;
  std::size_t size = 0;
  if (function(device, cache, &size, nullptr) != VK_SUCCESS || size == 0 ||
      size > 256 * 1024 * 1024)
    return;
  std::vector<uint8_t> data(size);
  if (function(device, cache, &size, data.data()) == VK_SUCCESS)
    save_pipeline_cache_data(data.data(), size);
}

void save_persistent_pipeline_cache(VkDevice device) {
  if (!device || !residency_enabled()) return;
  VkPipelineCache cache = VK_NULL_HANDLE;
  {
    std::scoped_lock lock(persistent_pipeline_cache_mutex);
    const auto found = persistent_pipeline_caches.find(device);
    if (found != persistent_pipeline_caches.end()) cache = found->second;
  }
  save_pipeline_cache_handle(device, cache);
}

void track_caller_pipeline_cache(VkDevice device, VkPipelineCache cache) {
  if (!device || !cache || !residency_enabled()) return;
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  caller_pipeline_caches[device].insert(cache);
}

void untrack_caller_pipeline_cache(VkDevice device, VkPipelineCache cache) {
  if (!device || !cache) return;
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  const auto found = caller_pipeline_caches.find(device);
  if (found == caller_pipeline_caches.end()) return;
  found->second.erase(cache);
  if (found->second.empty()) caller_pipeline_caches.erase(found);
}

void save_caller_pipeline_caches(VkDevice device) {
  if (!device || !residency_enabled()) return;
  std::vector<VkPipelineCache> caches;
  {
    std::scoped_lock lock(persistent_pipeline_cache_mutex);
    const auto found = caller_pipeline_caches.find(device);
    if (found != caller_pipeline_caches.end())
      caches.assign(found->second.begin(), found->second.end());
  }
  for (VkPipelineCache cache : caches) save_pipeline_cache_handle(device, cache);
  if (residency_trace_enabled() && !caches.empty())
    std::fprintf(stderr,
                 "nuah residency: snapshotted caller pipeline caches=%zu\n",
                 caches.size());
}

void clear_caller_pipeline_cache_tracking(VkDevice device) {
  if (!device) return;
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  caller_pipeline_caches.erase(device);
}

void destroy_persistent_pipeline_cache(
    VkDevice device, const VkAllocationCallbacks* allocator) {
  if (!device) return;
  save_persistent_pipeline_cache(device);
  VkPipelineCache cache = VK_NULL_HANDLE;
  {
    std::scoped_lock lock(persistent_pipeline_cache_mutex);
    const auto found = persistent_pipeline_caches.find(device);
    if (found != persistent_pipeline_caches.end()) {
      cache = found->second;
      persistent_pipeline_caches.erase(found);
    }
  }
  if (!cache) return;
  const auto function =
      host_function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache");
  if (function) function(device, cache, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(
    VkDevice device, const VkPipelineCacheCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkPipelineCache* cache) {
  const auto function =
      host_function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache");
  if (!function || !create_info || !cache)
    return VK_ERROR_INITIALIZATION_FAILED;
  VkPipelineCacheCreateInfo forwarded = *create_info;
  std::vector<uint8_t> disk_data;
  if (residency_enabled() && create_info->initialDataSize == 0 &&
      create_info->pNext == nullptr) {
    disk_data = load_pipeline_cache_data();
    if (!disk_data.empty()) {
      forwarded.initialDataSize = disk_data.size();
      forwarded.pInitialData = disk_data.data();
    }
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device, &forwarded, allocator, cache);
  if (result == VK_SUCCESS && cache && *cache != VK_NULL_HANDLE)
    track_caller_pipeline_cache(device, *cache);
  if (pipeline_trace_enabled()) {
    const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
    std::fprintf(stderr,
                 "nuah perf: vkCreatePipelineCache seed_bytes=%zu elapsed_us=%llu result=%d\n",
                 disk_data.size(), static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(
    VkDevice device, VkPipelineCache cache, std::size_t* data_size,
    void* data) {
  const auto function =
      host_function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
  if (!function || !data_size)
    return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = function(device, cache, data_size, data);
  if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && data &&
      *data_size != 0)
    save_pipeline_cache_data(data, *data_size);
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(
    VkDevice device, VkPipelineCache cache,
    const VkAllocationCallbacks* allocator) {
  save_pipeline_cache_handle(device, cache);
  untrack_caller_pipeline_cache(device, cache);
  const auto function =
      host_function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache");
  if (function) function(device, cache, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
  const auto function =
      host_function<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  VkPipelineCache effective_cache = cache;
  if (effective_cache == VK_NULL_HANDLE)
    effective_cache = persistent_pipeline_cache(device);
  if (residency_trace_enabled())
    std::fprintf(stderr,
                 "nuah residency: graphics cache_in=%p cache_used=%p count=%u\n",
                 reinterpret_cast<void*>(cache),
                 reinterpret_cast<void*>(effective_cache), count);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result =
      function(device, effective_cache, count, create_info, allocator, pipelines);
  if (result == VK_SUCCESS && residency_snapshot_enabled())
    save_pipeline_cache_handle(device, effective_cache);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (pipeline_trace_enabled() && elapsed_us >= 1000ULL) {
    std::fprintf(stderr,
                 "nuah perf: vkCreateGraphicsPipelines count=%u elapsed_us=%llu result=%d\n",
                 count, static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  }
  if (pipeline_callsite_trace_enabled() && elapsed_us >= 1000ULL) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    std::fprintf(stderr,
                 "nuah perf: vkCreateGraphicsPipelines caller=0x%llx count=%u elapsed_us=%llu result=%d\n",
                 static_cast<unsigned long long>(caller), count,
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
  const auto function =
      host_function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  VkPipelineCache effective_cache = cache;
  if (effective_cache == VK_NULL_HANDLE)
    effective_cache = persistent_pipeline_cache(device);
  if (residency_trace_enabled())
    std::fprintf(stderr,
                 "nuah residency: compute cache_in=%p cache_used=%p count=%u\n",
                 reinterpret_cast<void*>(cache),
                 reinterpret_cast<void*>(effective_cache), count);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result =
      function(device, effective_cache, count, create_info, allocator, pipelines);
  if (result == VK_SUCCESS && residency_snapshot_enabled())
    save_pipeline_cache_handle(device, effective_cache);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (pipeline_trace_enabled() && elapsed_us >= 1000ULL) {
    std::fprintf(stderr,
                 "nuah perf: vkCreateComputePipelines count=%u elapsed_us=%llu result=%d\n",
                 count, static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  }
  if (pipeline_callsite_trace_enabled() && elapsed_us >= 1000ULL) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    std::fprintf(stderr,
                 "nuah perf: vkCreateComputePipelines caller=0x%llx count=%u elapsed_us=%llu result=%d\n",
                 static_cast<unsigned long long>(caller), count,
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
  const auto function = host_function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (wait_trace_enabled() && elapsed_us >= 1000ULL)
    std::fprintf(stderr,
                 "nuah perf: vkQueueWaitIdle elapsed_us=%llu result=%d\n",
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
  const auto function = host_function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device);
  save_persistent_pipeline_cache(device);
  save_caller_pipeline_caches(device);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (wait_trace_enabled() && elapsed_us >= 1000ULL)
    std::fprintf(stderr,
                 "nuah perf: vkDeviceWaitIdle elapsed_us=%llu result=%d\n",
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks* allocator) {
  save_persistent_pipeline_cache(device);
  save_caller_pipeline_caches(device);
  clear_caller_pipeline_cache_tracking(device);
  destroy_persistent_pipeline_cache(device, allocator);
  const auto function = host_function<PFN_vkDestroyDevice>("vkDestroyDevice");
  if (function) function(device, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice device, uint32_t count, const VkFence* fences, VkBool32 wait_all,
    uint64_t timeout) {
  const auto function = host_function<PFN_vkWaitForFences>("vkWaitForFences");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device, count, fences, wait_all, timeout);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (wait_trace_enabled() && elapsed_us >= 1000ULL)
    std::fprintf(stderr,
                 "nuah perf: vkWaitForFences count=%u timeout=%llu elapsed_us=%llu result=%d\n",
                 count, static_cast<unsigned long long>(timeout),
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, uint32_t submit_count, const VkSubmitInfo* submits,
    VkFence fence) {
  const auto function = host_function<PFN_vkQueueSubmit>("vkQueueSubmit");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue, submit_count, submits, fence);
  if (submit_trace_enabled()) {
    uint64_t command_buffers = 0;
    for (uint32_t index = 0; index < submit_count; ++index)
      command_buffers += submits ? submits[index].commandBufferCount : 0;
    const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
    std::fprintf(stderr,
                 "nuah perf: vkQueueSubmit batches=%u command_buffers=%llu elapsed_us=%llu result=%d\n",
                 submit_count, static_cast<unsigned long long>(command_buffers),
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(
    VkQueue queue, uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence) {
  const auto function = host_function<PFN_vkQueueSubmit2>("vkQueueSubmit2");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue, submit_count, submits, fence);
  if (submit_trace_enabled()) {
    uint64_t command_buffers = 0;
    for (uint32_t index = 0; index < submit_count; ++index)
      command_buffers += submits ? submits[index].commandBufferInfoCount : 0;
    const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
    std::fprintf(stderr,
                 "nuah perf: vkQueueSubmit2 batches=%u command_buffers=%llu elapsed_us=%llu result=%d\n",
                 submit_count, static_cast<unsigned long long>(command_buffers),
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(
    VkCommandBuffer command_buffer, VkBuffer source, VkImage destination,
    VkImageLayout layout, uint32_t region_count,
    const VkBufferImageCopy* regions) {
  const auto function = host_function<PFN_vkCmdCopyBufferToImage>(
      "vkCmdCopyBufferToImage");
  if (!function) return;
  function(command_buffer, source, destination, layout, region_count, regions);
  if (copy_trace_enabled()) {
    uint64_t texels = 0;
    for (uint32_t index = 0; index < region_count; ++index) {
      const VkExtent3D extent = regions ? regions[index].imageExtent
                                        : VkExtent3D{0, 0, 0};
      texels += static_cast<uint64_t>(extent.width) * extent.height *
                extent.depth;
    }
    std::fprintf(stderr,
                 "nuah perf: vkCmdCopyBufferToImage regions=%u approx_texels=%llu\n",
                 region_count, static_cast<unsigned long long>(texels));
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* image_index) {
  const auto function =
      host_function<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t started_ns = monotonic_ns();
  const VkResult result =
      function(device, swapchain, timeout, semaphore, fence, image_index);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (wait_trace_enabled() && elapsed_us >= 1000ULL)
    std::fprintf(stderr,
                 "nuah perf: vkAcquireNextImageKHR timeout=%llu elapsed_us=%llu result=%d\n",
                 static_cast<unsigned long long>(timeout),
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  return result;
}

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

/* The Android linker’s bionic_dlsym() gives a provider’s `bionic_<symbol>`
 * export precedence over its ordinary export.  ATL also exposes a GTK-backed
 * bionic_vkCreateAndroidSurfaceKHR, so an explicit ATL provider would
 * otherwise receive Nuah’s SDL ANativeWindow and dereference it as a GTK
 * widget.  Keep the aliases at the Vulkan boundary: they are the same public
 * NDK entry points, but force dynamic Android lookups through Nuah’s
 * Wayland/SDL adapter. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char* name);

VKAPI_ATTR VkResult VKAPI_CALL bionic_vkCreateAndroidSurfaceKHR(
    VkInstance instance,
    const AndroidSurfaceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkSurfaceKHR* output) {
  return vkCreateAndroidSurfaceKHR(instance, create_info, allocator, output);
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
  normalize_swapchain_depth(capabilities);
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
  normalize_swapchain_depth(&capabilities->surfaceCapabilities);
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
          : requested_present_mode() == RequestedPresentMode::fifo_relaxed
                ? "fifo_relaxed"
                : requested_present_mode() == RequestedPresentMode::mailbox
                      ? "mailbox"
                      : "immediate";
  if (wanted_index == *count) {
    /* FIFO_RELAXED is commonly absent on Wayland WSI.  Leaving the complete
     * host list visible here lets Roblox pick its first entry (often
     * IMMEDIATE), which defeats the requested pacing policy and produces
     * visible cadence spikes.  Prefer ordinary FIFO as the safe compositor-
     * paced fallback; only leave the driver list untouched when even FIFO is
     * unavailable. */
    uint32_t fifo_index = *count;
    for (uint32_t index = 0; index < *count; ++index) {
      if (modes[index] == VK_PRESENT_MODE_FIFO_KHR) {
        fifo_index = index;
        break;
      }
    }
    if (fifo_index == *count) {
      trace_present_mode(mode_name, false);
      return result;
    }
    if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::fprintf(stderr,
                   "nuah vulkan: requested present mode=%s unavailable; "
                   "falling back to fifo\n",
                   mode_name);
    }
    wanted_index = fifo_index;
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

VKAPI_ATTR VkResult VKAPI_CALL bionic_vkCreateInstance(
    VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkInstance* output) {
  return vkCreateInstance(create_info, allocator, output);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL bionic_vkGetInstanceProcAddr(
    VkInstance instance, const char* name) {
  return vkGetInstanceProcAddr(instance, name);
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
  /* Pipeline residency is a production optimization, not merely a tracing
   * feature.  The old gate meant Roblox bypassed our persistent cache unless
   * NUAH_VULKAN_PIPELINE_TRACE=1 was set, so every normal launch paid the
   * driver compile cost again.  Keep tracing as an independent opt-in while
   * always interposing when residency is enabled (the default). */
  if (name && std::strcmp(name, "vkCreateGraphicsPipelines") == 0 &&
      (residency_enabled() || pipeline_trace_enabled())) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateGraphicsPipelines);
  }
  if (name && std::strcmp(name, "vkCreatePipelineCache") == 0 &&
      residency_enabled() &&
      host_function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreatePipelineCache);
  }
  if (name && std::strcmp(name, "vkGetPipelineCacheData") == 0 &&
      residency_enabled() &&
      host_function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPipelineCacheData);
  }
  if (name && std::strcmp(name, "vkDestroyPipelineCache") == 0 &&
      residency_enabled() &&
      host_function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipelineCache);
  }
  if (name && std::strcmp(name, "vkCreateComputePipelines") == 0 &&
      (residency_enabled() || pipeline_trace_enabled())) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateComputePipelines);
  }
  if (name && std::strcmp(name, "vkQueueWaitIdle") == 0 &&
      wait_trace_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueWaitIdle);
  }
  if (name && std::strcmp(name, "vkDeviceWaitIdle") == 0 &&
      (wait_trace_enabled() || residency_enabled())) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDeviceWaitIdle);
  }
  if (name && std::strcmp(name, "vkDestroyDevice") == 0 &&
      residency_enabled() &&
      host_function<PFN_vkDestroyDevice>("vkDestroyDevice")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDevice);
  }
  if (name && std::strcmp(name, "vkWaitForFences") == 0 &&
      wait_trace_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkWaitForFences);
  }
  if (name && std::strcmp(name, "vkQueueSubmit") == 0 &&
      submit_trace_enabled() &&
      host_function<PFN_vkQueueSubmit>("vkQueueSubmit")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueSubmit);
  }
  if (name && std::strcmp(name, "vkQueueSubmit2") == 0 &&
      submit_trace_enabled() &&
      host_function<PFN_vkQueueSubmit2>("vkQueueSubmit2")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueSubmit2);
  }
  if (name && std::strcmp(name, "vkCmdCopyBufferToImage") == 0 &&
      copy_trace_enabled() &&
      host_function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyBufferToImage);
  }
  if (name && std::strcmp(name, "vkAcquireNextImageKHR") == 0 &&
      wait_trace_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkAcquireNextImageKHR);
  }
  if (name &&
      std::strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(
        &vkGetPhysicalDeviceSurfacePresentModesKHR);
  }
  return function ? function(device, name) : nullptr;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties* memory_properties) {
  const auto function =
      host_function<PFN_vkGetPhysicalDeviceMemoryProperties>(
          "vkGetPhysicalDeviceMemoryProperties");
  if (!function || !memory_properties) return;
  function(physical_device, memory_properties);
  trace_memory_properties(physical_device, *memory_properties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physical_device,
    VkPhysicalDeviceMemoryProperties2* memory_properties) {
  const auto function =
      host_function<PFN_vkGetPhysicalDeviceMemoryProperties2>(
          "vkGetPhysicalDeviceMemoryProperties2");
  if (!function || !memory_properties) return;
  function(physical_device, memory_properties);
  trace_memory_properties(physical_device,
                          memory_properties->memoryProperties);
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
  if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0 &&
      host_function<PFN_vkGetPhysicalDeviceMemoryProperties>(
          "vkGetPhysicalDeviceMemoryProperties")) {
    return reinterpret_cast<PFN_vkVoidFunction>(
        &vkGetPhysicalDeviceMemoryProperties);
  }
  if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0 &&
      host_function<PFN_vkGetPhysicalDeviceMemoryProperties2>(
          "vkGetPhysicalDeviceMemoryProperties2")) {
    return reinterpret_cast<PFN_vkVoidFunction>(
        &vkGetPhysicalDeviceMemoryProperties2);
  }
  if (std::strcmp(name, "vkCreatePipelineCache") == 0 &&
      residency_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreatePipelineCache);
  }
  if (std::strcmp(name, "vkGetPipelineCacheData") == 0 &&
      residency_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPipelineCacheData);
  }
  if (std::strcmp(name, "vkDestroyPipelineCache") == 0 &&
      residency_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipelineCache);
  }
  if (std::strcmp(name, "vkCreateGraphicsPipelines") == 0 &&
      (residency_enabled() || pipeline_trace_enabled())) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateGraphicsPipelines);
  }
  if (std::strcmp(name, "vkCreateComputePipelines") == 0 &&
      (residency_enabled() || pipeline_trace_enabled())) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateComputePipelines);
  }
  if (std::strcmp(name, "vkQueuePresentKHR") == 0 && perf_trace_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHR);
  }
  if (std::strcmp(name, "vkDestroyDevice") == 0 && residency_enabled()) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDevice);
  }
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
  }
  const auto function =
      host_function<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
  return function ? function(instance, name) : nullptr;
}
}  // extern "C"
