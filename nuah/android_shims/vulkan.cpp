#include "nuah/native_window_bridge.h"
#include "nuah/android_abi_registry.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <array>
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
    if (!raw || !*raw) return RequestedPresentMode::mailbox;
    if (std::strcmp(raw, "fifo") == 0) return RequestedPresentMode::fifo;
    if (std::strcmp(raw, "fifo_relaxed") == 0)
      return RequestedPresentMode::fifo_relaxed;
    if (std::strcmp(raw, "mailbox") == 0) return RequestedPresentMode::mailbox;
    if (std::strcmp(raw, "immediate") == 0)
      return RequestedPresentMode::immediate;
    return RequestedPresentMode::mailbox;
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

/* A compact renderer-workload trace.  Unlike the older per-call tracing
 * switches, this is intended to stay enabled during a whole RIVALS run: it
 * only emits one summary per second. */
bool engine_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_ENGINE_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

/* Intel-format policy starts with evidence, not conversion.  This trace is
 * deliberately separate from NUAH_ENGINE_TRACE because vkCreateImage is only
 * needed while deciding whether Roblox is already using BCn/KTX2-friendly
 * images or expanding texture data to RGBA8. */
bool intel_format_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_INTEL_FORMAT_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool texture_upload_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_TEXTURE_UPLOAD_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool texture_upload_hash_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_TEXTURE_UPLOAD_HASH_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool texture_upload_dedup_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_TEXTURE_UPLOAD_DEDUP");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

/* ANV's descriptor path is expensive on Gen9: even a redundant bind can
 * rebuild the binding table (isl_gfx9_buffer_fill_state).  Keep this
 * mitigation opt-in because command-buffer state is owned by the application
 * and a wrong cache would be a rendering bug.  The cache only suppresses an
 * exact repeat of the most recent bind on a command buffer; all handles,
 * layout, set range, and dynamic offsets must match. */
bool descriptor_bind_dedup_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_DESCRIPTOR_BIND_DEDUP");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool descriptor_bind_dedup_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_DESCRIPTOR_BIND_DEDUP_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

/* Allocate a small number of same-layout sets ahead of the next request. The
 * sets remain real allocations from Roblox's descriptor pool; Nuah only
 * retains handles that the driver has already allocated. Disabled by default
 * because it trades descriptor-pool capacity for fewer allocator calls. */
uint32_t descriptor_alloc_batch_size() {
  static const uint32_t value = [] {
    const char* raw = std::getenv("NUAH_DESCRIPTOR_ALLOC_BATCH");
    if (!raw || !*raw) return 0U;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed < 1 || parsed > 16)
      return 0U;
    return static_cast<uint32_t>(parsed);
  }();
  return value;
}

bool descriptor_alloc_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_DESCRIPTOR_ALLOC_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

/* Per-present workload attribution for the frame-broker MVP.  A submission
 * before a present is evidence of new GPU work, not proof of new pixels; the
 * distinction is kept explicit in the telemetry. */
bool frame_work_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_FRAME_WORK_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool upload_fingerprint_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_UPLOAD_FINGERPRINT");
    if (!raw || !*raw) raw = std::getenv("NUAH_ISPC_UPLOAD_FINGERPRINT");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

void gcc_upload_fingerprint(const uint8_t* bytes, int32_t length,
                            uint64_t* result) {
  uint32_t first = 0x9e3779b1u;
  uint32_t second = 0x85ebca77u;
  for (int32_t index = 0; index < length; ++index) {
    const uint32_t position = static_cast<uint32_t>(index + 1);
    const uint32_t value = bytes[static_cast<uint32_t>(index)];
    uint32_t left = value + 0x9e3779b1u * position;
    uint32_t right = value + 0x85ebca77u * position;
    left ^= left >> 15;
    right ^= right >> 13;
    first += left * 0x27d4eb2du;
    second += right * 0x165667b1u;
  }
  result[0] = first;
  result[1] = second;
}

/* Opt-in mip residency clamp for the Intel path. This is deliberately a
 * sampler policy, not a fake format-capability report: Roblox keeps owning
 * KTX2/ETC2 decoding and UI samplers with no mip chain are unchanged. */
float texture_min_lod() {
  static const float value = [] {
    const char* raw = std::getenv("NUAH_TEXTURE_MIN_LOD");
    if (!raw || !*raw) return 0.0f;
    char* end = nullptr;
    const float parsed = std::strtof(raw, &end);
    if (end == raw || *end != '\0' || parsed < 0.0f || parsed > 4.0f)
      return 0.0f;
    return parsed;
  }();
  return value;
}

bool texture_min_lod_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_TEXTURE_MIN_LOD_TRACE");
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

/* Per-copy printf is itself capable of blocking the render thread while a
 * texture mip chain is recorded.  Keep the diagnostic opt-in, but rate-limit
 * it so enabling the trace cannot manufacture a frame hitch. */
bool copy_trace_log_slot() {
  static std::atomic<uint64_t> next_log_ns{0};
  const uint64_t now = monotonic_ns();
  uint64_t next = next_log_ns.load(std::memory_order_relaxed);
  while (now >= next) {
    if (next_log_ns.compare_exchange_weak(
            next, now + 1000000000ULL, std::memory_order_relaxed,
            std::memory_order_relaxed))
      return true;
  }
  return false;
}

enum class EngineEvent : std::size_t {
  image_create,
  copy_to_image,
  descriptor_allocate,
  descriptor_update,
  descriptor_free,
  descriptor_pool_create,
  descriptor_pool_reset,
  descriptor_bind,
  image_view_create,
  barrier,
  submit,
  wait_fence,
  acquire,
  count,
};

struct EngineAggregate {
  uint64_t calls = 0;
  uint64_t units = 0;
  uint64_t total_ns = 0;
  uint64_t max_ns = 0;
};

struct ImageFormatAggregate {
  uint64_t images = 0;
  uint64_t sampled_images = 0;
  uint64_t mip_levels = 0;
  uint32_t max_mip_levels = 0;
  uint64_t texels = 0;
  uint64_t estimated_bytes = 0;
};

struct TrackedImage {
  uint64_t id = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0;
  bool dedup_eligible = false;
};

struct TrackedBuffer {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize memory_offset = 0;
};

struct MappedMemory {
  const std::byte* base = nullptr;
  VkDeviceSize offset = 0;
  VkDeviceSize size = 0;
};

struct DescriptorBindSnapshot {
  VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  uint32_t first_set = 0;
  std::vector<VkDescriptorSet> descriptor_sets;
  std::vector<uint32_t> dynamic_offsets;
  bool valid = false;
};

struct DescriptorBindState {
  std::mutex mutex;
  std::unordered_map<uintptr_t, DescriptorBindSnapshot> last_bind;
};

struct DescriptorPoolLayoutKey {
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;

  bool operator==(const DescriptorPoolLayoutKey& other) const noexcept {
    return pool == other.pool && layout == other.layout;
  }
};

struct DescriptorPoolLayoutHash {
  std::size_t operator()(const DescriptorPoolLayoutKey& key) const noexcept {
    const auto p = reinterpret_cast<uintptr_t>(key.pool);
    const auto l = reinterpret_cast<uintptr_t>(key.layout);
    return p ^ (l + 0x9e3779b97f4a7c15ULL + (p << 6) + (p >> 2));
  }
};

struct DescriptorAllocCache {
  std::mutex mutex;
  std::unordered_map<DescriptorPoolLayoutKey, std::vector<VkDescriptorSet>,
                     DescriptorPoolLayoutHash>
      sets;
};

DescriptorAllocCache& descriptor_alloc_cache() {
  static DescriptorAllocCache value;
  return value;
}

std::atomic<uint64_t> descriptor_alloc_calls{0};
std::atomic<uint64_t> descriptor_alloc_driver_calls{0};
std::atomic<uint64_t> descriptor_alloc_driver_sets{0};
std::atomic<uint64_t> descriptor_alloc_cache_hits{0};
std::atomic<uint64_t> descriptor_alloc_batched_sets{0};

bool take_cached_descriptor_set(VkDescriptorPool pool,
                                VkDescriptorSetLayout layout,
                                VkDescriptorSet* output) {
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  const auto it = cache.sets.find({pool, layout});
  if (it == cache.sets.end() || it->second.empty()) return false;
  *output = it->second.back();
  it->second.pop_back();
  return true;
}

void restore_cached_descriptor_sets(
    VkDescriptorPool pool,
    const std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>>& sets) {
  if (sets.empty()) return;
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  for (const auto& [layout, descriptor_set] : sets)
    cache.sets[{pool, layout}].push_back(descriptor_set);
}

std::vector<VkDescriptorSet> retain_batched_descriptor_sets(
    VkDescriptorPool pool,
    const std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>>& sets) {
  constexpr std::size_t kPerLayoutLimit = 32;
  std::vector<VkDescriptorSet> discard;
  if (sets.empty()) return discard;
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  for (const auto& [layout, descriptor_set] : sets) {
    auto& retained = cache.sets[{pool, layout}];
    if (retained.size() < kPerLayoutLimit)
      retained.push_back(descriptor_set);
    else
      discard.push_back(descriptor_set);
  }
  return discard;
}

void clear_cached_descriptor_pool(VkDescriptorPool pool) {
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  for (auto it = cache.sets.begin(); it != cache.sets.end();) {
    if (it->first.pool == pool) {
      it = cache.sets.erase(it);
    } else {
      ++it;
    }
  }
}

void clear_all_cached_descriptor_sets() {
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  cache.sets.clear();
}

void maybe_report_descriptor_alloc() {
  if (!descriptor_alloc_trace_enabled()) return;
  static std::atomic<uint64_t> next_report_ns{0};
  const uint64_t now = monotonic_ns();
  uint64_t next = next_report_ns.load(std::memory_order_relaxed);
  if (now < next || !next_report_ns.compare_exchange_strong(
                         next, now + 1000000000ULL,
                         std::memory_order_relaxed,
                         std::memory_order_relaxed))
    return;
  std::fprintf(stderr,
               "nuah descriptor-alloc: requests=%llu driver_calls=%llu driver_sets=%llu cache_hits=%llu batched_sets=%llu\n",
               static_cast<unsigned long long>(
                   descriptor_alloc_calls.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(descriptor_alloc_driver_calls.load(
                   std::memory_order_relaxed)),
               static_cast<unsigned long long>(descriptor_alloc_driver_sets.load(
                   std::memory_order_relaxed)),
               static_cast<unsigned long long>(descriptor_alloc_cache_hits.load(
                   std::memory_order_relaxed)),
               static_cast<unsigned long long>(descriptor_alloc_batched_sets.load(
                   std::memory_order_relaxed)));
}

bool command_state_dedup_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_COMMAND_STATE_DEDUP");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

bool command_state_dedup_trace_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_COMMAND_STATE_DEDUP_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}

enum class CommandStateKind : uint8_t {
  pipeline,
  index_buffer,
  vertex_buffers,
  vertex_buffers2,
};

struct CommandStateSnapshot {
  CommandStateKind kind = CommandStateKind::pipeline;
  VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkBuffer index_buffer = VK_NULL_HANDLE;
  VkDeviceSize index_offset = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  uint32_t first_binding = 0;
  std::vector<VkBuffer> buffers;
  std::vector<VkDeviceSize> offsets;
  std::vector<VkDeviceSize> sizes;
  std::vector<VkDeviceSize> strides;
  bool sizes_present = false;
  bool strides_present = false;
  bool valid = false;
};

struct CommandStateCache {
  std::mutex mutex;
  /* Roblox commonly interleaves pipeline, index, and vertex binds.  Keeping
   * only one last state therefore misses the exact-repeat bind after another
   * bind kind has intervened.  Keep one independent snapshot per kind; this
   * still suppresses only byte-for-byte equivalent Vulkan state calls. */
  struct PerCommandBuffer {
    std::array<CommandStateSnapshot, 4> state;
  };
  std::unordered_map<uintptr_t, PerCommandBuffer> last_state;
};

CommandStateCache& command_state_cache() {
  static CommandStateCache value;
  return value;
}

std::atomic<uint64_t> command_state_calls{0};
std::atomic<uint64_t> command_state_suppressed{0};

bool command_state_matches(const CommandStateSnapshot& previous,
                           const CommandStateSnapshot& current) {
  return previous.valid && previous.kind == current.kind &&
         previous.bind_point == current.bind_point &&
         previous.pipeline == current.pipeline &&
         previous.index_buffer == current.index_buffer &&
         previous.index_offset == current.index_offset &&
         previous.index_type == current.index_type &&
         previous.first_binding == current.first_binding &&
         previous.sizes_present == current.sizes_present &&
         previous.strides_present == current.strides_present &&
         previous.buffers == current.buffers &&
         previous.offsets == current.offsets &&
         previous.sizes == current.sizes &&
         previous.strides == current.strides;
}

bool command_state_is_duplicate(VkCommandBuffer command_buffer,
                                const CommandStateSnapshot& current) {
  CommandStateCache& cache = command_state_cache();
  std::scoped_lock lock(cache.mutex);
  const auto iterator =
      cache.last_state.find(reinterpret_cast<uintptr_t>(command_buffer));
  if (iterator == cache.last_state.end()) return false;
  const std::size_t index = static_cast<std::size_t>(current.kind);
  if (index >= iterator->second.state.size()) return false;
  return command_state_matches(iterator->second.state[index], current);
}

void remember_command_state(VkCommandBuffer command_buffer,
                            CommandStateSnapshot snapshot) {
  snapshot.valid = true;
  CommandStateCache& cache = command_state_cache();
  std::scoped_lock lock(cache.mutex);
  const std::size_t index = static_cast<std::size_t>(snapshot.kind);
  if (index >= cache.last_state[reinterpret_cast<uintptr_t>(command_buffer)]
                   .state.size())
    return;
  cache.last_state[reinterpret_cast<uintptr_t>(command_buffer)].state[index] =
      std::move(snapshot);
}

void clear_command_state(VkCommandBuffer command_buffer) {
  CommandStateCache& cache = command_state_cache();
  std::scoped_lock lock(cache.mutex);
  cache.last_state.erase(reinterpret_cast<uintptr_t>(command_buffer));
}

void clear_all_command_state() {
  CommandStateCache& cache = command_state_cache();
  std::scoped_lock lock(cache.mutex);
  cache.last_state.clear();
}

void maybe_report_command_state() {
  if (!command_state_dedup_trace_enabled()) return;
  static std::atomic<uint64_t> next_report_ns{0};
  const uint64_t now = monotonic_ns();
  uint64_t next = next_report_ns.load(std::memory_order_relaxed);
  if (now < next || !next_report_ns.compare_exchange_strong(
                         next, now + 1000000000ULL,
                         std::memory_order_relaxed,
                         std::memory_order_relaxed))
    return;
  std::fprintf(stderr,
               "nuah command-state: calls=%llu suppressed=%llu\n",
               static_cast<unsigned long long>(
                   command_state_calls.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(command_state_suppressed.load(
                   std::memory_order_relaxed)));
}

DescriptorBindState& descriptor_bind_state() {
  static DescriptorBindState value;
  return value;
}

std::atomic<uint64_t> descriptor_bind_calls{0};
std::atomic<uint64_t> descriptor_bind_suppressed{0};

bool descriptor_bind_matches(const DescriptorBindSnapshot& previous,
                             VkPipelineBindPoint bind_point,
                             VkPipelineLayout layout, uint32_t first_set,
                             uint32_t descriptor_set_count,
                             const VkDescriptorSet* descriptor_sets,
                             uint32_t dynamic_offset_count,
                             const uint32_t* dynamic_offsets) {
  if (!previous.valid || previous.bind_point != bind_point ||
      previous.layout != layout || previous.first_set != first_set ||
      previous.descriptor_sets.size() != descriptor_set_count ||
      previous.dynamic_offsets.size() != dynamic_offset_count)
    return false;
  if (descriptor_set_count &&
      std::memcmp(previous.descriptor_sets.data(), descriptor_sets,
                  sizeof(VkDescriptorSet) * descriptor_set_count) != 0)
    return false;
  if (dynamic_offset_count &&
      std::memcmp(previous.dynamic_offsets.data(), dynamic_offsets,
                  sizeof(uint32_t) * dynamic_offset_count) != 0)
    return false;
  return true;
}

void remember_descriptor_bind(VkCommandBuffer command_buffer,
                              VkPipelineBindPoint bind_point,
                              VkPipelineLayout layout, uint32_t first_set,
                              uint32_t descriptor_set_count,
                              const VkDescriptorSet* descriptor_sets,
                              uint32_t dynamic_offset_count,
                              const uint32_t* dynamic_offsets) {
  DescriptorBindState& state = descriptor_bind_state();
  std::scoped_lock lock(state.mutex);
  DescriptorBindSnapshot& snapshot =
      state.last_bind[reinterpret_cast<uintptr_t>(command_buffer)];
  snapshot.bind_point = bind_point;
  snapshot.layout = layout;
  snapshot.first_set = first_set;
  snapshot.descriptor_sets.clear();
  if (descriptor_set_count)
    snapshot.descriptor_sets.assign(descriptor_sets,
                                   descriptor_sets + descriptor_set_count);
  snapshot.dynamic_offsets.clear();
  if (dynamic_offset_count)
    snapshot.dynamic_offsets.assign(dynamic_offsets,
                                    dynamic_offsets + dynamic_offset_count);
  snapshot.valid = true;
}

void clear_descriptor_bind(VkCommandBuffer command_buffer) {
  DescriptorBindState& state = descriptor_bind_state();
  std::scoped_lock lock(state.mutex);
  state.last_bind.erase(reinterpret_cast<uintptr_t>(command_buffer));
}

void clear_all_descriptor_binds() {
  DescriptorBindState& state = descriptor_bind_state();
  std::scoped_lock lock(state.mutex);
  state.last_bind.clear();
}

void maybe_report_descriptor_bind_dedup() {
  if (!descriptor_bind_dedup_trace_enabled()) return;
  static std::atomic<uint64_t> next_report_ns{0};
  const uint64_t now = monotonic_ns();
  uint64_t next = next_report_ns.load(std::memory_order_relaxed);
  if (now < next || !next_report_ns.compare_exchange_strong(
                         next, now + 1000000000ULL,
                         std::memory_order_relaxed,
                         std::memory_order_relaxed))
    return;
  std::fprintf(stderr,
               "nuah descriptor-dedup: binds=%llu suppressed=%llu\n",
               static_cast<unsigned long long>(
                   descriptor_bind_calls.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(descriptor_bind_suppressed.load(
                   std::memory_order_relaxed)));
}

struct UploadSubresource {
  uint64_t image_id = 0;
  uint32_t mip_level = 0;
  uint32_t layer = 0;
  VkImageAspectFlags aspect = 0;
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;

  bool operator==(const UploadSubresource& other) const = default;
};

struct UploadSubresourceHash {
  std::size_t operator()(const UploadSubresource& value) const {
    std::size_t result = std::hash<uint64_t>{}(value.image_id);
    const auto combine = [&result](uint64_t part) {
      result ^= std::hash<uint64_t>{}(part) + 0x9e3779b97f4a7c15ULL +
                (result << 6) + (result >> 2);
    };
    combine(value.mip_level);
    combine(value.layer);
    combine(value.aspect);
    combine(static_cast<uint32_t>(value.x));
    combine(static_cast<uint32_t>(value.y));
    combine(static_cast<uint32_t>(value.z));
    combine(value.width);
    combine(value.height);
    combine(value.depth);
    return result;
  }
};

struct UploadAggregate {
  uint64_t regions = 0;
  uint64_t bytes = 0;
  uint64_t repeated_regions = 0;
  uint64_t repeated_bytes = 0;
  uint64_t hashed_regions = 0;
  uint64_t identical_regions = 0;
  uint64_t identical_bytes = 0;
  uint64_t suppressed_regions = 0;
  uint64_t suppressed_bytes = 0;
  uint64_t fingerprint_calls = 0;
  uint64_t fingerprint_ns = 0;
};

struct PendingFrameWork {
  uint64_t submit_calls = 0;
  uint64_t submit_batches = 0;
  uint64_t command_buffers = 0;
  uint64_t requested_copy_regions = 0;
  uint64_t forwarded_copy_regions = 0;
  uint64_t forwarded_copy_bytes = 0;
};

struct FrameWorkWindow {
  uint64_t presents = 0;
  uint64_t presents_with_submit = 0;
  uint64_t presents_without_submit = 0;
  uint64_t submit_calls = 0;
  uint64_t submit_batches = 0;
  uint64_t command_buffers = 0;
  uint64_t requested_copy_regions = 0;
  uint64_t forwarded_copy_regions = 0;
  uint64_t forwarded_copy_bytes = 0;
  uint64_t max_command_buffers_per_present = 0;
  uint64_t max_forwarded_copy_bytes_per_present = 0;
};

struct FrameWorkMetrics {
  std::mutex mutex;
  PendingFrameWork pending;
  FrameWorkWindow window;
  uint64_t next_report_ns = 0;
};

FrameWorkMetrics& frame_work_metrics() {
  static FrameWorkMetrics value;
  return value;
}

struct EngineTrace {
  std::mutex mutex;
  std::array<EngineAggregate, static_cast<std::size_t>(EngineEvent::count)>
      events{};
  std::unordered_map<VkFormat, ImageFormatAggregate> image_formats;
  std::unordered_map<uintptr_t, TrackedImage> images;
  std::unordered_map<uintptr_t, TrackedBuffer> buffers;
  std::unordered_map<uintptr_t, MappedMemory> mapped_memory;
  std::unordered_map<UploadSubresource, uint64_t, UploadSubresourceHash>
      seen_uploads;
  UploadAggregate uploads;
  uint64_t next_image_id = 1;
  uint64_t next_report_ns = 0;
};

EngineTrace& engine_trace() {
  static EngineTrace value;
  return value;
}

const char* engine_event_name(EngineEvent event) {
  switch (event) {
    case EngineEvent::image_create: return "image_create";
    case EngineEvent::copy_to_image: return "copy_to_image";
    case EngineEvent::descriptor_allocate: return "descriptor_allocate";
    case EngineEvent::descriptor_update: return "descriptor_update";
    case EngineEvent::descriptor_free: return "descriptor_free";
    case EngineEvent::descriptor_pool_create: return "descriptor_pool_create";
    case EngineEvent::descriptor_pool_reset: return "descriptor_pool_reset";
    case EngineEvent::descriptor_bind: return "descriptor_bind";
    case EngineEvent::image_view_create: return "image_view_create";
    case EngineEvent::barrier: return "barrier";
    case EngineEvent::submit: return "submit";
    case EngineEvent::wait_fence: return "wait_fence";
    case EngineEvent::acquire: return "acquire";
    case EngineEvent::count: break;
  }
  return "unknown";
}

const char* format_name(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8_SRGB";
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return "BC1_RGB";
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return "BC1_RGB_SRGB";
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA";
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1_RGBA_SRGB";
    case VK_FORMAT_BC2_UNORM_BLOCK: return "BC2";
    case VK_FORMAT_BC2_SRGB_BLOCK: return "BC2_SRGB";
    case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB";
    case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4";
    case VK_FORMAT_BC4_SNORM_BLOCK: return "BC4_SNORM";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5";
    case VK_FORMAT_BC5_SNORM_BLOCK: return "BC5_SNORM";
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: return "BC6H_UFLOAT";
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return "BC6H_SFLOAT";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7_SRGB";
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return "ETC2_RGB";
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: return "ETC2_RGB_SRGB";
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: return "ETC2_RGBA1";
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return "ETC2_RGBA1_SRGB";
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: return "ETC2_RGBA8";
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return "ETC2_RGBA8_SRGB";
    default: return "other";
  }
}

uint32_t format_block_bytes(VkFormat format) {
  switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
      return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
      return 16;
    default:
      return 0;
  }
}

uint64_t estimate_image_bytes(const VkImageCreateInfo& info) {
  const uint32_t compressed_block_bytes = format_block_bytes(info.format);
  const uint32_t mip_levels = std::max(info.mipLevels, 1u);
  uint64_t total = 0;
  for (uint32_t mip = 0; mip < mip_levels; ++mip) {
    const uint64_t width = std::max(info.extent.width >> mip, 1u);
    const uint64_t height = std::max(info.extent.height >> mip, 1u);
    const uint64_t depth = std::max(info.extent.depth >> mip, 1u);
    if (compressed_block_bytes) {
      total += ((width + 3) / 4) * ((height + 3) / 4) * depth *
               compressed_block_bytes * info.arrayLayers;
    } else if (info.format == VK_FORMAT_R8G8B8A8_UNORM ||
               info.format == VK_FORMAT_R8G8B8A8_SRGB) {
      total += width * height * depth * 4 * info.arrayLayers;
    }
  }
  return total;
}

uint64_t estimate_region_bytes(VkFormat format, VkExtent3D extent) {
  const uint32_t block_bytes = format_block_bytes(format);
  if (block_bytes) {
    return ((static_cast<uint64_t>(extent.width) + 3) / 4) *
           ((static_cast<uint64_t>(extent.height) + 3) / 4) * extent.depth *
           block_bytes;
  }
  switch (format) {
    case VK_FORMAT_R8_UNORM: return static_cast<uint64_t>(extent.width) * extent.height * extent.depth;
    case VK_FORMAT_R8G8_UNORM: return static_cast<uint64_t>(extent.width) * extent.height * extent.depth * 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
      return static_cast<uint64_t>(extent.width) * extent.height * extent.depth * 4;
    default: return 0;
  }
}

void record_image_format(const VkImageCreateInfo* info) {
  if (!intel_format_trace_enabled() || !info) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  ImageFormatAggregate& aggregate = trace.image_formats[info->format];
  ++aggregate.images;
  if (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ++aggregate.sampled_images;
  const uint32_t mip_levels = std::max(info->mipLevels, 1u);
  aggregate.mip_levels += mip_levels;
  aggregate.max_mip_levels = std::max(aggregate.max_mip_levels, mip_levels);
  aggregate.texels += static_cast<uint64_t>(info->extent.width) *
                      info->extent.height * info->extent.depth *
                      info->arrayLayers;
  aggregate.estimated_bytes += estimate_image_bytes(*info);
}

void record_tracked_image(VkImage image, const VkImageCreateInfo* info) {
  if ((!texture_upload_trace_enabled() && !frame_work_trace_enabled()) ||
      image == VK_NULL_HANDLE || !info)
    return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const VkImageUsageFlags forbidden = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  // A sampled texture may legitimately also be used as a transfer source for
  // blits. That does not make an unchanged CPU upload necessary; only images
  // writable by graphics/compute remain excluded from the guarded deduper.
  const bool eligible = (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
      (info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && !(info->usage & forbidden);
  trace.images[reinterpret_cast<uintptr_t>(image)] = TrackedImage{
      trace.next_image_id++, info->format, info->usage, eligible};
}

uint64_t estimate_frame_copy_bytes(VkImage image, uint32_t region_count,
                                   const VkBufferImageCopy* regions) {
  if (!frame_work_trace_enabled() || !regions) return 0;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const auto image_it = trace.images.find(reinterpret_cast<uintptr_t>(image));
  if (image_it == trace.images.end()) return 0;
  uint64_t bytes = 0;
  for (uint32_t index = 0; index < region_count; ++index) {
    bytes += estimate_region_bytes(image_it->second.format,
                                   regions[index].imageExtent) *
             regions[index].imageSubresource.layerCount;
  }
  return bytes;
}

void record_frame_submit(uint32_t batches, uint64_t command_buffers) {
  if (!frame_work_trace_enabled()) return;
  FrameWorkMetrics& metrics = frame_work_metrics();
  std::scoped_lock lock(metrics.mutex);
  ++metrics.pending.submit_calls;
  metrics.pending.submit_batches += batches;
  metrics.pending.command_buffers += command_buffers;
}

void record_frame_copy(uint32_t requested_regions, uint32_t forwarded_regions,
                       uint64_t forwarded_bytes) {
  if (!frame_work_trace_enabled()) return;
  FrameWorkMetrics& metrics = frame_work_metrics();
  std::scoped_lock lock(metrics.mutex);
  metrics.pending.requested_copy_regions += requested_regions;
  metrics.pending.forwarded_copy_regions += forwarded_regions;
  metrics.pending.forwarded_copy_bytes += forwarded_bytes;
}

void record_frame_present(uint64_t now) {
  if (!frame_work_trace_enabled()) return;
  FrameWorkMetrics& metrics = frame_work_metrics();
  FrameWorkWindow report{};
  bool ready = false;
  {
    std::scoped_lock lock(metrics.mutex);
    PendingFrameWork pending = metrics.pending;
    metrics.pending = {};
    ++metrics.window.presents;
    if (pending.submit_calls) ++metrics.window.presents_with_submit;
    else ++metrics.window.presents_without_submit;
    metrics.window.submit_calls += pending.submit_calls;
    metrics.window.submit_batches += pending.submit_batches;
    metrics.window.command_buffers += pending.command_buffers;
    metrics.window.requested_copy_regions += pending.requested_copy_regions;
    metrics.window.forwarded_copy_regions += pending.forwarded_copy_regions;
    metrics.window.forwarded_copy_bytes += pending.forwarded_copy_bytes;
    metrics.window.max_command_buffers_per_present = std::max(
        metrics.window.max_command_buffers_per_present, pending.command_buffers);
    metrics.window.max_forwarded_copy_bytes_per_present = std::max(
        metrics.window.max_forwarded_copy_bytes_per_present,
        pending.forwarded_copy_bytes);
    if (!metrics.next_report_ns) metrics.next_report_ns = now + 1000000000ULL;
    if (now < metrics.next_report_ns) return;
    report = metrics.window;
    metrics.window = {};
    metrics.next_report_ns = now + 1000000000ULL;
    ready = true;
  }
  if (!ready) return;
  std::fprintf(stderr,
               "nuah frame-work: presents=%llu with_submit=%llu without_submit=%llu submit_calls=%llu batches=%llu command_buffers=%llu command_buffers_per_present=%llu copy_regions_requested=%llu copy_regions_forwarded=%llu copy_mib_forwarded=%llu max_command_buffers_per_present=%llu max_copy_mib_per_present=%llu\n",
               static_cast<unsigned long long>(report.presents),
               static_cast<unsigned long long>(report.presents_with_submit),
               static_cast<unsigned long long>(report.presents_without_submit),
               static_cast<unsigned long long>(report.submit_calls),
               static_cast<unsigned long long>(report.submit_batches),
               static_cast<unsigned long long>(report.command_buffers),
               static_cast<unsigned long long>(report.presents
                   ? report.command_buffers / report.presents : 0),
               static_cast<unsigned long long>(report.requested_copy_regions),
               static_cast<unsigned long long>(report.forwarded_copy_regions),
               static_cast<unsigned long long>(report.forwarded_copy_bytes /
                                               (1024ULL * 1024ULL)),
               static_cast<unsigned long long>(report.max_command_buffers_per_present),
               static_cast<unsigned long long>(
                   report.max_forwarded_copy_bytes_per_present /
                   (1024ULL * 1024ULL)));
}

uint64_t source_hash(EngineTrace& trace, VkBuffer buffer,
                     const VkBufferImageCopy& region, uint64_t bytes);

bool should_suppress_upload(VkImage image, const VkBufferImageCopy& region,
                            uint64_t content_hash) {
  if (!texture_upload_dedup_enabled() || !texture_upload_hash_trace_enabled())
    return false;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const auto image_it = trace.images.find(reinterpret_cast<uintptr_t>(image));
  if (image_it == trace.images.end() || !image_it->second.dedup_eligible)
    return false;
  const uint64_t bytes = estimate_region_bytes(image_it->second.format,
                                                region.imageExtent) *
      region.imageSubresource.layerCount;
  if (!content_hash) return false;
  const UploadSubresource key{image_it->second.id,
                              region.imageSubresource.mipLevel,
                              region.imageSubresource.baseArrayLayer,
                              region.imageSubresource.aspectMask,
                              region.imageOffset.x, region.imageOffset.y,
                              region.imageOffset.z, region.imageExtent.width,
                              region.imageExtent.height, region.imageExtent.depth};
  const auto existing = trace.seen_uploads.find(key);
  if (existing == trace.seen_uploads.end() || existing->second != content_hash)
    return false;
  ++trace.uploads.suppressed_regions;
  trace.uploads.suppressed_bytes += bytes;
  return true;
}

std::vector<uint64_t> collect_upload_hashes(VkBuffer source, VkImage image,
                                            uint32_t region_count,
                                            const VkBufferImageCopy* regions) {
  std::vector<uint64_t> result;
  if (!texture_upload_hash_trace_enabled() || !regions || !region_count)
    return result;
  result.resize(region_count);
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const auto image_it = trace.images.find(reinterpret_cast<uintptr_t>(image));
  if (image_it == trace.images.end()) return result;
  for (uint32_t index = 0; index < region_count; ++index) {
    /* Do not fingerprint every cold upload.  The first upload for a
     * subresource cannot be suppressed, so hashing it only adds CPU work to
     * the render path.  Hash only when this exact destination region has
     * already been observed; the first repeated upload establishes the
     * content fingerprint and a later match can then be suppressed safely. */
    const VkBufferImageCopy& region = regions[index];
    const UploadSubresource key{
        image_it->second.id,
        region.imageSubresource.mipLevel,
        region.imageSubresource.baseArrayLayer,
        region.imageSubresource.aspectMask,
        region.imageOffset.x,
        region.imageOffset.y,
        region.imageOffset.z,
        region.imageExtent.width,
        region.imageExtent.height,
        region.imageExtent.depth};
    if (trace.seen_uploads.find(key) == trace.seen_uploads.end()) continue;
    const uint64_t bytes = estimate_region_bytes(image_it->second.format,
                                                  region.imageExtent) *
        region.imageSubresource.layerCount;
    result[index] = source_hash(trace, source, regions[index], bytes);
  }
  return result;
}

void forget_tracked_image(VkImage image) {
  if (!texture_upload_trace_enabled() || image == VK_NULL_HANDLE) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  trace.images.erase(reinterpret_cast<uintptr_t>(image));
}

uint64_t hash_upload_bytes(const std::byte* bytes, uint64_t size) {
  if (upload_fingerprint_enabled() && size <= 0x7fff'ffffULL) {
    std::array<uint64_t, 2> fingerprint{};
    gcc_upload_fingerprint(reinterpret_cast<const uint8_t*>(bytes),
                           static_cast<int32_t>(size), fingerprint.data());
    return fingerprint[0] ^ ((fingerprint[1] << 17) | (fingerprint[1] >> 47));
  }
  uint64_t hash = 1469598103934665603ULL;
  for (uint64_t index = 0; index < size; ++index) {
    hash ^= static_cast<uint8_t>(bytes[index]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t source_hash(EngineTrace& trace, VkBuffer buffer,
                     const VkBufferImageCopy& region, uint64_t bytes) {
  if (!texture_upload_hash_trace_enabled() || bytes == 0 ||
      bytes > 8ULL * 1024ULL * 1024ULL || region.bufferRowLength != 0 ||
      region.bufferImageHeight != 0)
    return 0;
  const auto buffer_it = trace.buffers.find(reinterpret_cast<uintptr_t>(buffer));
  if (buffer_it == trace.buffers.end()) return 0;
  const auto memory_it = trace.mapped_memory.find(
      reinterpret_cast<uintptr_t>(buffer_it->second.memory));
  if (memory_it == trace.mapped_memory.end() ||
      memory_it->second.size == VK_WHOLE_SIZE)
    return 0;
  const VkDeviceSize start = buffer_it->second.memory_offset + region.bufferOffset;
  if (start < memory_it->second.offset) return 0;
  const VkDeviceSize relative = start - memory_it->second.offset;
  if (relative > memory_it->second.size || bytes > memory_it->second.size - relative)
    return 0;
  const uint64_t started_ns = monotonic_ns();
  const uint64_t result = hash_upload_bytes(memory_it->second.base + relative, bytes);
  ++trace.uploads.fingerprint_calls;
  trace.uploads.fingerprint_ns += monotonic_ns() - started_ns;
  return result;
}

void track_buffer_binding(VkBuffer buffer, VkDeviceMemory memory,
                          VkDeviceSize memory_offset) {
  if (!texture_upload_hash_trace_enabled()) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  trace.buffers[reinterpret_cast<uintptr_t>(buffer)] =
      TrackedBuffer{memory, memory_offset};
}

void forget_tracked_buffer(VkBuffer buffer) {
  if (!texture_upload_hash_trace_enabled()) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  trace.buffers.erase(reinterpret_cast<uintptr_t>(buffer));
}

void track_mapped_memory(VkDeviceMemory memory, VkDeviceSize offset,
                         VkDeviceSize size, void* data) {
  if (!texture_upload_hash_trace_enabled() || !data) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  trace.mapped_memory[reinterpret_cast<uintptr_t>(memory)] =
      MappedMemory{static_cast<const std::byte*>(data), offset, size};
}

void forget_mapped_memory(VkDeviceMemory memory) {
  if (!texture_upload_hash_trace_enabled()) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  trace.mapped_memory.erase(reinterpret_cast<uintptr_t>(memory));
}

void record_texture_upload(VkImage image, uint32_t region_count,
                           const VkBufferImageCopy* regions,
                           const std::vector<uint64_t>& content_hashes) {
  if (!texture_upload_trace_enabled() || !regions) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const auto image_it = trace.images.find(reinterpret_cast<uintptr_t>(image));
  if (image_it == trace.images.end() ||
      !(image_it->second.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    return;
  for (uint32_t index = 0; index < region_count; ++index) {
    const VkBufferImageCopy& region = regions[index];
    const uint64_t bytes =
        estimate_region_bytes(image_it->second.format, region.imageExtent) *
        region.imageSubresource.layerCount;
    UploadSubresource key{image_it->second.id,
                          region.imageSubresource.mipLevel,
                          region.imageSubresource.baseArrayLayer,
                          region.imageSubresource.aspectMask,
                          region.imageOffset.x,
                          region.imageOffset.y,
                          region.imageOffset.z,
                          region.imageExtent.width,
                          region.imageExtent.height,
                          region.imageExtent.depth};
    ++trace.uploads.regions;
    trace.uploads.bytes += bytes;
    const uint64_t content_hash = index < content_hashes.size()
        ? content_hashes[index] : 0;
    if (content_hash) ++trace.uploads.hashed_regions;
    const auto [existing, inserted] = trace.seen_uploads.emplace(key, content_hash);
    if (!inserted) {
      ++trace.uploads.repeated_regions;
      trace.uploads.repeated_bytes += bytes;
      if (content_hash && existing->second == content_hash) {
        ++trace.uploads.identical_regions;
        trace.uploads.identical_bytes += bytes;
      }
      existing->second = content_hash;
    }
  }
  // Bound memory during long play sessions.  A reset merely starts a new
  // duplicate-observation window; it never changes an image or upload.
  if (trace.seen_uploads.size() > 200000) trace.seen_uploads.clear();
}

void record_engine_event(EngineEvent event, uint64_t elapsed_ns,
                         uint64_t units = 1) {
  if (!engine_trace_enabled() && !intel_format_trace_enabled() &&
      !texture_upload_trace_enabled())
    return;
  EngineTrace& trace = engine_trace();
  std::array<EngineAggregate, static_cast<std::size_t>(EngineEvent::count)>
      report{};
  std::unordered_map<VkFormat, ImageFormatAggregate> format_report;
  UploadAggregate upload_report{};
  bool ready = false;
  const uint64_t now = monotonic_ns();
  {
    std::scoped_lock lock(trace.mutex);
    EngineAggregate& aggregate = trace.events[static_cast<std::size_t>(event)];
    ++aggregate.calls;
    aggregate.units += units;
    aggregate.total_ns += elapsed_ns;
    aggregate.max_ns = std::max(aggregate.max_ns, elapsed_ns);
    if (trace.next_report_ns == 0) trace.next_report_ns = now + 1000000000ULL;
    if (now < trace.next_report_ns) return;
    report = trace.events;
    trace.events = {};
    format_report.swap(trace.image_formats);
    upload_report = trace.uploads;
    trace.uploads = {};
    trace.next_report_ns = now + 1000000000ULL;
    ready = true;
  }
  if (!ready) return;
  for (std::size_t index = 0; index < report.size(); ++index) {
    const EngineAggregate& aggregate = report[index];
    if (!aggregate.calls) continue;
    std::fprintf(stderr,
                 "nuah engine: vk=%s calls=%llu units=%llu total_us=%llu avg_us=%llu max_us=%llu\n",
                 engine_event_name(static_cast<EngineEvent>(index)),
                 static_cast<unsigned long long>(aggregate.calls),
                 static_cast<unsigned long long>(aggregate.units),
                 static_cast<unsigned long long>(aggregate.total_ns / 1000ULL),
                 static_cast<unsigned long long>(aggregate.total_ns /
                                                 aggregate.calls / 1000ULL),
                 static_cast<unsigned long long>(aggregate.max_ns / 1000ULL));
  }
  for (const auto& [format, aggregate] : format_report) {
    if (!aggregate.images) continue;
    std::fprintf(stderr,
                 "nuah intel-format: format=%s vk_format=%d images=%llu sampled=%llu mip_levels=%llu max_mips=%u texels=%llu estimated_mib=%llu\n",
                 format_name(format), static_cast<int>(format),
                 static_cast<unsigned long long>(aggregate.images),
                 static_cast<unsigned long long>(aggregate.sampled_images),
                 static_cast<unsigned long long>(aggregate.mip_levels),
                 aggregate.max_mip_levels,
                 static_cast<unsigned long long>(aggregate.texels),
                 static_cast<unsigned long long>(aggregate.estimated_bytes /
                                                 (1024ULL * 1024ULL)));
  }
  if (upload_report.regions) {
    std::fprintf(stderr,
                 "nuah texture-upload: regions=%llu estimated_mib=%llu repeated_regions=%llu repeated_mib=%llu hashed_regions=%llu identical_regions=%llu identical_mib=%llu suppressed_regions=%llu suppressed_mib=%llu fingerprint_calls=%llu fingerprint_us=%llu\n",
                 static_cast<unsigned long long>(upload_report.regions),
                 static_cast<unsigned long long>(upload_report.bytes /
                                                 (1024ULL * 1024ULL)),
                 static_cast<unsigned long long>(upload_report.repeated_regions),
                 static_cast<unsigned long long>(upload_report.repeated_bytes /
                                                 (1024ULL * 1024ULL)),
                 static_cast<unsigned long long>(upload_report.hashed_regions),
                 static_cast<unsigned long long>(upload_report.identical_regions),
                 static_cast<unsigned long long>(upload_report.identical_bytes /
                                                 (1024ULL * 1024ULL)),
                 static_cast<unsigned long long>(upload_report.suppressed_regions),
                 static_cast<unsigned long long>(upload_report.suppressed_bytes /
                                                 (1024ULL * 1024ULL)),
                 static_cast<unsigned long long>(upload_report.fingerprint_calls),
                 static_cast<unsigned long long>(upload_report.fingerprint_ns / 1000ULL));
  }
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
 * host driver's advertised range authoritative, use that depth by default,
 * and allow an explicit A/B override when the minimum is lower and the
 * driver permits the deeper queue.  The extra image is important on the
 * Intel/Wayland FIFO path: with three images, vkAcquireNextImageKHR can block
 * on a drmSyncobj timeline for a whole refresh before Roblox can record the
 * next frame. */
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
    /* Match the Android client even for quality/1920x1080 launches.  An
     * explicit NUAH_VULKAN_MIN_IMAGE_COUNT=3 remains available for a direct
     * latency comparison, but is not a useful hitch profile on this host. */
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

/* Callers on the render path must store the result in a function-local
 * `static`. Repeating dlsym() for every descriptor/copy/submit call takes the
 * dynamic-loader lock and was itself observed as a 60–130 ms hitch. */

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
  if (!directory || !*directory)
    directory = std::getenv("MESA_SHADER_CACHE_DIR");
  if (!directory || !*directory) {
    const char* home = std::getenv("HOME");
    if (home && *home) {
      static const auto default_dir =
          std::filesystem::path(home) / ".local" / "share" / "nuah" / "mesa-shader-cache";
      return default_dir / "nuah-vk-pipeline-cache.bin";
    }
    return {};
  }
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
  static const auto function =
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
  const bool trace = pipeline_trace_enabled() || pipeline_callsite_trace_enabled();
  const uint64_t started_ns = trace ? monotonic_ns() : 0;
  VkResult result =
      function(device, effective_cache, count, create_info, allocator, pipelines);
  if (result != VK_SUCCESS && effective_cache != cache) {
    result = function(device, cache, count, create_info, allocator, pipelines);
  }
  if (result == VK_SUCCESS && residency_snapshot_enabled())
    save_pipeline_cache_handle(device, effective_cache);
  if (trace) {
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
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
  static const auto function =
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
  const bool trace = pipeline_trace_enabled() || pipeline_callsite_trace_enabled();
  const uint64_t started_ns = trace ? monotonic_ns() : 0;
  VkResult result =
      function(device, effective_cache, count, create_info, allocator, pipelines);
  if (result != VK_SUCCESS && effective_cache != cache) {
    result = function(device, cache, count, create_info, allocator, pipelines);
  }
  if (result == VK_SUCCESS && residency_snapshot_enabled())
    save_pipeline_cache_handle(device, effective_cache);
  if (trace) {
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
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
  static const auto function = host_function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!wait_trace_enabled()) return function(queue);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (elapsed_us >= 1000ULL)
    std::fprintf(stderr,
                 "nuah perf: vkQueueWaitIdle elapsed_us=%llu result=%d\n",
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
  static const auto function = host_function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!wait_trace_enabled()) {
    const VkResult result = function(device);
    save_persistent_pipeline_cache(device);
    save_caller_pipeline_caches(device);
    return result;
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device);
  save_persistent_pipeline_cache(device);
  save_caller_pipeline_caches(device);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (elapsed_us >= 1000ULL)
    std::fprintf(stderr,
                 "nuah perf: vkDeviceWaitIdle elapsed_us=%llu result=%d\n",
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks* allocator) {
  if (descriptor_bind_dedup_enabled()) clear_all_descriptor_binds();
  if (command_state_dedup_enabled()) clear_all_command_state();
  if (descriptor_alloc_batch_size()) clear_all_cached_descriptor_sets();
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
  static const auto function = host_function<PFN_vkWaitForFences>("vkWaitForFences");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!wait_trace_enabled() && !engine_trace_enabled()) {
    return function(device, count, fences, wait_all, timeout);
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device, count, fences, wait_all, timeout);
  const uint64_t elapsed_ns = monotonic_ns() - started_ns;
  record_engine_event(EngineEvent::wait_fence, elapsed_ns, count);
  const uint64_t elapsed_us = elapsed_ns / 1000ULL;
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
  static const auto function = host_function<PFN_vkQueueSubmit>("vkQueueSubmit");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!submit_trace_enabled() && !engine_trace_enabled() &&
      !frame_work_trace_enabled()) {
    return function(queue, submit_count, submits, fence);
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue, submit_count, submits, fence);
  uint64_t command_buffers = 0;
  for (uint32_t index = 0; index < submit_count; ++index)
    command_buffers += submits ? submits[index].commandBufferCount : 0;
  if (result == VK_SUCCESS) record_frame_submit(submit_count, command_buffers);
  record_engine_event(EngineEvent::submit, monotonic_ns() - started_ns,
                      command_buffers);
  if (submit_trace_enabled()) {
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
  static const auto function = host_function<PFN_vkQueueSubmit2>("vkQueueSubmit2");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!submit_trace_enabled() && !engine_trace_enabled() &&
      !frame_work_trace_enabled()) {
    return function(queue, submit_count, submits, fence);
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue, submit_count, submits, fence);
  uint64_t command_buffers = 0;
  for (uint32_t index = 0; index < submit_count; ++index)
    command_buffers += submits ? submits[index].commandBufferInfoCount : 0;
  if (result == VK_SUCCESS) record_frame_submit(submit_count, command_buffers);
  record_engine_event(EngineEvent::submit, monotonic_ns() - started_ns,
                      command_buffers);
  if (submit_trace_enabled()) {
    const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
    std::fprintf(stderr,
                 "nuah perf: vkQueueSubmit2 batches=%u command_buffers=%llu elapsed_us=%llu result=%d\n",
                 submit_count, static_cast<unsigned long long>(command_buffers),
                 static_cast<unsigned long long>(elapsed_us),
                 static_cast<int>(result));
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImage* image) {
  static const auto function = host_function<PFN_vkCreateImage>("vkCreateImage");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!intel_format_trace_enabled() && !texture_upload_dedup_enabled() &&
      !engine_trace_enabled()) {
    return function(device, create_info, allocator, image);
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device, create_info, allocator, image);
  if (result == VK_SUCCESS) {
    record_image_format(create_info);
    record_tracked_image(image ? *image : VK_NULL_HANDLE, create_info);
  }
  record_engine_event(EngineEvent::image_create, monotonic_ns() - started_ns);
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
  forget_tracked_image(image);
  const auto function = host_function<PFN_vkDestroyImage>("vkDestroyImage");
  if (function) function(device, image, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
    VkDeviceSize memory_offset) {
  const auto function = host_function<PFN_vkBindBufferMemory>("vkBindBufferMemory");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = function(device, buffer, memory, memory_offset);
  if (result == VK_SUCCESS) track_buffer_binding(buffer, memory, memory_offset);
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator) {
  forget_tracked_buffer(buffer);
  const auto function = host_function<PFN_vkDestroyBuffer>("vkDestroyBuffer");
  if (function) function(device, buffer, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void** data) {
  const auto function = host_function<PFN_vkMapMemory>("vkMapMemory");
  if (!function) return VK_ERROR_MEMORY_MAP_FAILED;
  const VkResult result = function(device, memory, offset, size, flags, data);
  if (result == VK_SUCCESS) track_mapped_memory(memory, offset, size, data ? *data : nullptr);
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
  forget_mapped_memory(memory);
  const auto function = host_function<PFN_vkUnmapMemory>("vkUnmapMemory");
  if (function) function(device, memory);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(
    VkCommandBuffer command_buffer, VkBuffer source, VkImage destination,
    VkImageLayout layout, uint32_t region_count,
    const VkBufferImageCopy* regions) {
  static const auto function = host_function<PFN_vkCmdCopyBufferToImage>(
      "vkCmdCopyBufferToImage");
  if (!function) return;
  if (!texture_upload_dedup_enabled() && !texture_upload_trace_enabled() &&
      !copy_trace_enabled() && !engine_trace_enabled() &&
      !frame_work_trace_enabled()) {
    function(command_buffer, source, destination, layout, region_count, regions);
    return;
  }
  const uint64_t started_ns = monotonic_ns();
  const std::vector<uint64_t> content_hashes =
      collect_upload_hashes(source, destination, region_count, regions);
  std::vector<VkBufferImageCopy> retained_regions;
  const VkBufferImageCopy* forwarded_regions = regions;
  uint32_t forwarded_count = region_count;
  if (texture_upload_dedup_enabled() && regions && region_count) {
    retained_regions.reserve(region_count);
    for (uint32_t index = 0; index < region_count; ++index) {
      const uint64_t content_hash = index < content_hashes.size()
          ? content_hashes[index] : 0;
      if (!should_suppress_upload(destination, regions[index], content_hash))
        retained_regions.push_back(regions[index]);
    }
    forwarded_count = static_cast<uint32_t>(retained_regions.size());
    forwarded_regions = retained_regions.empty() ? nullptr : retained_regions.data();
  }
  const uint64_t forwarded_bytes = estimate_frame_copy_bytes(
      destination, forwarded_count, forwarded_regions);
  if (forwarded_count)
    function(command_buffer, source, destination, layout, forwarded_count,
             forwarded_regions);
  uint64_t texels = 0;
  for (uint32_t index = 0; index < region_count; ++index) {
    const VkExtent3D extent = regions ? regions[index].imageExtent
                                      : VkExtent3D{0, 0, 0};
    texels += static_cast<uint64_t>(extent.width) * extent.height *
              extent.depth;
  }
  record_texture_upload(destination, region_count, regions, content_hashes);
  record_frame_copy(region_count, forwarded_count, forwarded_bytes);
  record_engine_event(EngineEvent::copy_to_image, monotonic_ns() - started_ns,
                      texels);
  if (copy_trace_enabled() && copy_trace_log_slot()) {
    std::fprintf(stderr,
                 "nuah perf: vkCmdCopyBufferToImage regions=%u approx_texels=%llu (rate-limited)\n",
                 region_count, static_cast<unsigned long long>(texels));
  }
}

/* Roblox uses the Vulkan 1.3 copy command on the current ANV path.  Keep the
 * same conservative duplicate-upload policy as the legacy entry point: a
 * region is eligible only for sampled images, a mapped source buffer, and a
 * previously observed identical fingerprint.  The first occurrence always
 * reaches the driver.  This is deliberately a command-recording filter; it
 * does not rewrite formats or split arbitrary copies, which would change
 * Vulkan synchronization semantics. */
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(
    VkCommandBuffer command_buffer,
    const VkCopyBufferToImageInfo2* copy_info) {
  static const auto function = host_function<PFN_vkCmdCopyBufferToImage2>(
      "vkCmdCopyBufferToImage2");
  if (!function || !copy_info) return;
  if (!texture_upload_dedup_enabled() && !texture_upload_trace_enabled() &&
      !copy_trace_enabled() && !engine_trace_enabled() &&
      !frame_work_trace_enabled()) {
    function(command_buffer, copy_info);
    return;
  }

  const uint64_t started_ns = monotonic_ns();
  std::vector<VkBufferImageCopy> legacy_regions;
  legacy_regions.reserve(copy_info->regionCount);
  for (uint32_t index = 0; index < copy_info->regionCount; ++index) {
    const VkBufferImageCopy2& region = copy_info->pRegions[index];
    legacy_regions.push_back(VkBufferImageCopy{
        region.bufferOffset,
        region.bufferRowLength,
        region.bufferImageHeight,
        region.imageSubresource,
        region.imageOffset,
        region.imageExtent});
  }
  const std::vector<uint64_t> content_hashes = collect_upload_hashes(
      copy_info->srcBuffer, copy_info->dstImage, copy_info->regionCount,
      legacy_regions.empty() ? nullptr : legacy_regions.data());

  std::vector<VkBufferImageCopy2> retained_regions;
  std::vector<VkBufferImageCopy> retained_legacy_regions;
  const VkBufferImageCopy2* forwarded_regions = copy_info->pRegions;
  uint32_t forwarded_count = copy_info->regionCount;
  if (texture_upload_dedup_enabled() && copy_info->pRegions &&
      copy_info->regionCount) {
    retained_regions.reserve(copy_info->regionCount);
    retained_legacy_regions.reserve(copy_info->regionCount);
    for (uint32_t index = 0; index < copy_info->regionCount; ++index) {
      const uint64_t content_hash = index < content_hashes.size()
          ? content_hashes[index] : 0;
      if (!should_suppress_upload(copy_info->dstImage, legacy_regions[index],
                                  content_hash)) {
        retained_regions.push_back(copy_info->pRegions[index]);
        retained_legacy_regions.push_back(legacy_regions[index]);
      }
    }
    forwarded_count = static_cast<uint32_t>(retained_regions.size());
    forwarded_regions = retained_regions.empty() ? nullptr :
        retained_regions.data();
  }

  VkCopyBufferToImageInfo2 forwarded_info = *copy_info;
  forwarded_info.regionCount = forwarded_count;
  forwarded_info.pRegions = forwarded_regions;
  const uint64_t forwarded_bytes = estimate_frame_copy_bytes(
      copy_info->dstImage, forwarded_count,
      forwarded_count && !retained_regions.empty()
          ? retained_legacy_regions.data()
          : legacy_regions.empty() ? nullptr : legacy_regions.data());
  if (forwarded_count) {
    function(command_buffer, &forwarded_info);
  }
  record_texture_upload(copy_info->dstImage, copy_info->regionCount,
                        legacy_regions.empty() ? nullptr : legacy_regions.data(),
                        content_hashes);
  record_frame_copy(copy_info->regionCount, forwarded_count, forwarded_bytes);
  record_engine_event(EngineEvent::copy_to_image, monotonic_ns() - started_ns,
                      copy_info->regionCount);
  if (copy_trace_enabled() && copy_trace_log_slot()) {
    std::fprintf(stderr,
                 "nuah perf: vkCmdCopyBufferToImage2 regions=%u forwarded=%u (rate-limited)\n",
                 copy_info->regionCount, forwarded_count);
  }
}

/* Roblox's current render path also records image-to-image copies through the
 * Vulkan 1.3 entry point.  Keep this wrapper observational: unlike buffer
 * uploads, an image copy can carry ordering/layout semantics, so Nuah must not
 * silently remove it without proving the operation is redundant.  Capturing
 * it here lets the hitch trace correlate blorp_copy with the exact copy
 * workload instead of leaving that path invisible. */
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(
    VkCommandBuffer command_buffer, const VkCopyImageInfo2* copy_info) {
  static const auto function =
      host_function<PFN_vkCmdCopyImage2>("vkCmdCopyImage2");
  if (!function || !copy_info) return;
  if (!copy_trace_enabled() && !engine_trace_enabled()) {
    function(command_buffer, copy_info);
    return;
  }
  const uint64_t started_ns = monotonic_ns();
  function(command_buffer, copy_info);
  uint64_t texels = 0;
  for (uint32_t index = 0; index < copy_info->regionCount; ++index) {
    const VkExtent3D extent = copy_info->pRegions
        ? copy_info->pRegions[index].extent : VkExtent3D{0, 0, 0};
    texels += static_cast<uint64_t>(extent.width) * extent.height *
              extent.depth;
  }
  record_engine_event(EngineEvent::copy_to_image,
                      monotonic_ns() - started_ns,
                      copy_info->regionCount);
  if (copy_trace_enabled() && copy_trace_log_slot())
    std::fprintf(stderr,
                 "nuah perf: vkCmdCopyImage2 regions=%u approx_texels=%llu\n",
                 copy_info->regionCount,
                 static_cast<unsigned long long>(texels));
}

/* The trace resolves the legacy vkCmdCopyImage entry point in Mesa even
 * though the driver eventually executes its internal CmdCopyImage2 path.
 * Observe that boundary as well; this remains a pure forwarder because image
 * copies can carry synchronization and layout ordering semantics. */
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(
    VkCommandBuffer command_buffer, VkImage source, VkImageLayout source_layout,
    VkImage destination, VkImageLayout destination_layout, uint32_t region_count,
    const VkImageCopy* regions) {
  static const auto function =
      host_function<PFN_vkCmdCopyImage>("vkCmdCopyImage");
  if (!function) return;
  if (!copy_trace_enabled() && !engine_trace_enabled()) {
    function(command_buffer, source, source_layout, destination,
             destination_layout, region_count, regions);
    return;
  }
  const uint64_t started_ns = monotonic_ns();
  function(command_buffer, source, source_layout, destination,
           destination_layout, region_count, regions);
  uint64_t texels = 0;
  for (uint32_t index = 0; index < region_count; ++index) {
    const VkExtent3D extent = regions ? regions[index].extent
                                      : VkExtent3D{0, 0, 0};
    texels += static_cast<uint64_t>(extent.width) * extent.height *
              extent.depth;
  }
  record_engine_event(EngineEvent::copy_to_image,
                      monotonic_ns() - started_ns, region_count);
  if (copy_trace_enabled() && copy_trace_log_slot())
    std::fprintf(stderr,
                 "nuah perf: vkCmdCopyImage regions=%u approx_texels=%llu\n",
                 region_count, static_cast<unsigned long long>(texels));
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo* allocate_info,
    VkDescriptorSet* descriptor_sets) {
  static const auto function = host_function<PFN_vkAllocateDescriptorSets>(
      "vkAllocateDescriptorSets");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const uint32_t batch_size = descriptor_alloc_batch_size();
  const bool trace = descriptor_alloc_trace_enabled() || engine_trace_enabled();
  if (!batch_size && !trace) {
    return function(device, allocate_info, descriptor_sets);
  }
  ++descriptor_alloc_calls;
  const uint64_t started_ns = monotonic_ns();
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  const bool batchable =
      batch_size && allocate_info && descriptor_sets &&
      allocate_info->descriptorSetCount && allocate_info->pSetLayouts &&
      !allocate_info->pNext;
  if (!batchable) {
    ++descriptor_alloc_driver_calls;
    descriptor_alloc_driver_sets +=
        allocate_info ? allocate_info->descriptorSetCount : 0;
    result = function(device, allocate_info, descriptor_sets);
  } else {
    const uint32_t requested_count = allocate_info->descriptorSetCount;
    std::vector<uint32_t> missing_indices;
    std::vector<VkDescriptorSetLayout> missing_layouts;
    std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>> consumed;
    missing_indices.reserve(requested_count);
    missing_layouts.reserve(requested_count);
    consumed.reserve(requested_count);
    for (uint32_t index = 0; index < requested_count; ++index) {
      VkDescriptorSet cached = VK_NULL_HANDLE;
      const VkDescriptorSetLayout layout = allocate_info->pSetLayouts[index];
      if (take_cached_descriptor_set(allocate_info->descriptorPool, layout,
                                     &cached)) {
        descriptor_sets[index] = cached;
        consumed.emplace_back(layout, cached);
        ++descriptor_alloc_cache_hits;
      } else {
        missing_indices.push_back(index);
        missing_layouts.push_back(layout);
      }
    }

    if (missing_indices.empty()) {
      result = VK_SUCCESS;
    } else {
      std::vector<VkDescriptorSetLayout> forwarded_layouts = missing_layouts;
      const uint32_t extra_count = std::min<uint32_t>(
          batch_size, static_cast<uint32_t>(missing_layouts.size()));
      for (uint32_t index = 0; index < extra_count; ++index)
        forwarded_layouts.push_back(missing_layouts[index]);
      std::vector<VkDescriptorSet> forwarded_sets(forwarded_layouts.size(),
                                                   VK_NULL_HANDLE);
      VkDescriptorSetAllocateInfo forwarded_info = *allocate_info;
      forwarded_info.descriptorSetCount =
          static_cast<uint32_t>(forwarded_layouts.size());
      forwarded_info.pSetLayouts = forwarded_layouts.data();
      ++descriptor_alloc_driver_calls;
      descriptor_alloc_driver_sets += forwarded_info.descriptorSetCount;
      result = function(device, &forwarded_info, forwarded_sets.data());
      if (result != VK_SUCCESS && extra_count) {
        /* A pool may have room for the request but not the speculative batch.
         * Retry the exact request; Vulkan guarantees no sets are allocated on
         * an allocation failure. */
        forwarded_info.descriptorSetCount =
            static_cast<uint32_t>(missing_layouts.size());
        forwarded_info.pSetLayouts = missing_layouts.data();
        forwarded_sets.resize(missing_layouts.size());
        ++descriptor_alloc_driver_calls;
        descriptor_alloc_driver_sets += forwarded_info.descriptorSetCount;
        result = function(device, &forwarded_info, forwarded_sets.data());
      }
      if (result == VK_SUCCESS) {
        for (std::size_t index = 0; index < missing_indices.size(); ++index)
          descriptor_sets[missing_indices[index]] = forwarded_sets[index];
        const std::size_t allocated_count = missing_layouts.size();
        if (forwarded_sets.size() > allocated_count) {
          std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>> extras;
          extras.reserve(forwarded_sets.size() - allocated_count);
          for (std::size_t index = allocated_count;
               index < forwarded_sets.size(); ++index)
            extras.emplace_back(forwarded_layouts[index], forwarded_sets[index]);
          const std::vector<VkDescriptorSet> discard =
              retain_batched_descriptor_sets(allocate_info->descriptorPool,
                                             extras);
          if (!discard.empty()) {
            static const auto free_function =
                host_function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets");
            if (free_function)
              free_function(device, allocate_info->descriptorPool,
                            static_cast<uint32_t>(discard.size()),
                            discard.data());
          }
          descriptor_alloc_batched_sets += extras.size() - discard.size();
        }
      } else {
        restore_cached_descriptor_sets(allocate_info->descriptorPool, consumed);
      }
    }
  }
  record_engine_event(EngineEvent::descriptor_allocate,
                      monotonic_ns() - started_ns,
                      allocate_info ? allocate_info->descriptorSetCount : 0);
  maybe_report_descriptor_alloc();
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(
    VkDevice device, VkDescriptorPool descriptor_pool,
    uint32_t descriptor_set_count, const VkDescriptorSet* descriptor_sets) {
  static const auto function = host_function<PFN_vkFreeDescriptorSets>(
      "vkFreeDescriptorSets");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!engine_trace_enabled()) {
    return function(device, descriptor_pool, descriptor_set_count, descriptor_sets);
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device, descriptor_pool,
                                   descriptor_set_count, descriptor_sets);
  record_engine_event(EngineEvent::descriptor_free, monotonic_ns() - started_ns,
                      descriptor_set_count);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDescriptorPool* descriptor_pool) {
  static const auto function = host_function<PFN_vkCreateDescriptorPool>(
      "vkCreateDescriptorPool");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!engine_trace_enabled()) {
    return function(device, create_info, allocator, descriptor_pool);
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device, create_info, allocator,
                                   descriptor_pool);
  record_engine_event(EngineEvent::descriptor_pool_create,
                      monotonic_ns() - started_ns,
                      create_info ? create_info->maxSets : 0);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(
    VkDevice device, VkDescriptorPool descriptor_pool,
    VkDescriptorPoolResetFlags flags) {
  static const auto function = host_function<PFN_vkResetDescriptorPool>(
      "vkResetDescriptorPool");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const bool trace = engine_trace_enabled();
  const uint64_t started_ns = trace ? monotonic_ns() : 0;
  const VkResult result = function(device, descriptor_pool, flags);
  if (descriptor_alloc_batch_size() && result == VK_SUCCESS)
    clear_cached_descriptor_pool(descriptor_pool);
  if (trace) {
    record_engine_event(EngineEvent::descriptor_pool_reset,
                        monotonic_ns() - started_ns);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(
    VkDevice device, VkDescriptorPool descriptor_pool,
    const VkAllocationCallbacks* allocator) {
  static const auto function = host_function<PFN_vkDestroyDescriptorPool>(
      "vkDestroyDescriptorPool");
  if (!function) return;
  function(device, descriptor_pool, allocator);
  if (descriptor_alloc_batch_size())
    clear_cached_descriptor_pool(descriptor_pool);
}

// Descriptor writes are a CPU-side driver cost on ANV.  Measure them before
// considering any cache: Vulkan permits a later write to change the same set,
// so skipping calls without proving complete write identity would be unsafe.
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(
    VkDevice device, uint32_t descriptor_write_count,
    const VkWriteDescriptorSet* descriptor_writes,
    uint32_t descriptor_copy_count,
    const VkCopyDescriptorSet* descriptor_copies) {
  static const auto function = host_function<PFN_vkUpdateDescriptorSets>(
      "vkUpdateDescriptorSets");
  if (!function) return;
  if (!engine_trace_enabled()) {
    function(device, descriptor_write_count, descriptor_writes,
             descriptor_copy_count, descriptor_copies);
    return;
  }
  const uint64_t started_ns = monotonic_ns();
  function(device, descriptor_write_count, descriptor_writes,
           descriptor_copy_count, descriptor_copies);
  record_engine_event(EngineEvent::descriptor_update,
                      monotonic_ns() - started_ns,
                      static_cast<uint64_t>(descriptor_write_count) +
                          descriptor_copy_count);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImageView* image_view) {
  static const auto function =
      host_function<PFN_vkCreateImageView>("vkCreateImageView");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!engine_trace_enabled()) {
    return function(device, create_info, allocator, image_view);
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device, create_info, allocator, image_view);
  record_engine_event(EngineEvent::image_view_create,
                      monotonic_ns() - started_ns);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(
    VkDevice device, const VkSamplerCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkSampler* sampler) {
  static const auto function = host_function<PFN_vkCreateSampler>("vkCreateSampler");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const float min_lod = texture_min_lod();
  if (!create_info || min_lod == 0.0f ||
      create_info->unnormalizedCoordinates == VK_TRUE ||
      create_info->maxLod <= create_info->minLod + min_lod) {
    return function(device, create_info, allocator, sampler);
  }
  VkSamplerCreateInfo adjusted = *create_info;
  adjusted.minLod = std::min(create_info->maxLod,
                             create_info->minLod + min_lod);
  adjusted.mipLodBias += min_lod;
  static std::atomic<uint64_t> adjusted_samplers{0};
  const uint64_t number = ++adjusted_samplers;
  if (texture_min_lod_trace_enabled() && number <= 8) {
    std::fprintf(stderr,
                 "nuah texture-lod: sampler=%llu old_min_lod=%.2f new_min_lod=%.2f old_bias=%.2f new_bias=%.2f\n",
                 static_cast<unsigned long long>(number), create_info->minLod,
                 adjusted.minLod, create_info->mipLodBias, adjusted.mipLodBias);
  }
  return function(device, &adjusted, allocator, sampler);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer command_buffer,
    const VkCommandBufferBeginInfo* begin_info) {
  static const auto function = host_function<PFN_vkBeginCommandBuffer>(
      "vkBeginCommandBuffer");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const bool dedup_desc = descriptor_bind_dedup_enabled();
  const bool dedup_cmd = command_state_dedup_enabled();
  const VkResult result = function(command_buffer, begin_info);
  if (dedup_desc) clear_descriptor_bind(command_buffer);
  if (dedup_cmd) clear_command_state(command_buffer);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(
    VkCommandBuffer command_buffer) {
  static const auto function =
      host_function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const bool dedup_desc = descriptor_bind_dedup_enabled();
  const bool dedup_cmd = command_state_dedup_enabled();
  const VkResult result = function(command_buffer);
  if (dedup_desc) clear_descriptor_bind(command_buffer);
  if (dedup_cmd) clear_command_state(command_buffer);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(
    VkCommandBuffer command_buffer, VkCommandBufferResetFlags flags) {
  static const auto function =
      host_function<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  const bool dedup_desc = descriptor_bind_dedup_enabled();
  const bool dedup_cmd = command_state_dedup_enabled();
  const VkResult result = function(command_buffer, flags);
  if (dedup_desc && result == VK_SUCCESS)
    clear_descriptor_bind(command_buffer);
  if (dedup_cmd && result == VK_SUCCESS)
    clear_command_state(command_buffer);
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice device, VkCommandPool command_pool, uint32_t command_buffer_count,
    const VkCommandBuffer* command_buffers) {
  static const auto function = host_function<PFN_vkFreeCommandBuffers>(
      "vkFreeCommandBuffers");
  if (!function) return;
  function(device, command_pool, command_buffer_count, command_buffers);
  if (!command_buffers) return;
  if (descriptor_bind_dedup_enabled()) {
    DescriptorBindState& state = descriptor_bind_state();
    std::scoped_lock lock(state.mutex);
    for (uint32_t index = 0; index < command_buffer_count; ++index)
      state.last_bind.erase(reinterpret_cast<uintptr_t>(command_buffers[index]));
  }
  if (command_state_dedup_enabled()) {
    CommandStateCache& command_cache = command_state_cache();
    std::scoped_lock command_lock(command_cache.mutex);
    for (uint32_t index = 0; index < command_buffer_count; ++index)
      command_cache.last_state.erase(
          reinterpret_cast<uintptr_t>(command_buffers[index]));
  }
}

VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(
    VkCommandBuffer command_buffer, uint32_t command_buffer_count,
    const VkCommandBuffer* command_buffers) {
  static const auto function = host_function<PFN_vkCmdExecuteCommands>(
      "vkCmdExecuteCommands");
  if (!function) return;
  function(command_buffer, command_buffer_count, command_buffers);
  /* A secondary command buffer may change descriptor state in the primary.
   * Invalidate rather than attempting to inspect secondary contents. */
  if (descriptor_bind_dedup_enabled()) clear_descriptor_bind(command_buffer);
  if (command_state_dedup_enabled()) clear_command_state(command_buffer);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipeline pipeline) {
  static const auto function =
      host_function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
  if (!function) return;
  const bool dedup = command_state_dedup_enabled();
  if (!dedup) {
    function(command_buffer, bind_point, pipeline);
    return;
  }
  CommandStateSnapshot snapshot;
  snapshot.kind = CommandStateKind::pipeline;
  snapshot.bind_point = bind_point;
  snapshot.pipeline = pipeline;
  ++command_state_calls;
  if (command_state_is_duplicate(command_buffer, snapshot)) {
    ++command_state_suppressed;
    maybe_report_command_state();
    return;
  }
  function(command_buffer, bind_point, pipeline);
  remember_command_state(command_buffer, std::move(snapshot));
  maybe_report_command_state();
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(
    VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset,
    VkIndexType index_type) {
  static const auto function =
      host_function<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
  if (!function) return;
  const bool dedup = command_state_dedup_enabled();
  if (!dedup) {
    function(command_buffer, buffer, offset, index_type);
    return;
  }
  CommandStateSnapshot snapshot;
  snapshot.kind = CommandStateKind::index_buffer;
  snapshot.index_buffer = buffer;
  snapshot.index_offset = offset;
  snapshot.index_type = index_type;
  ++command_state_calls;
  if (command_state_is_duplicate(command_buffer, snapshot)) {
    ++command_state_suppressed;
    maybe_report_command_state();
    return;
  }
  function(command_buffer, buffer, offset, index_type);
  remember_command_state(command_buffer, std::move(snapshot));
  maybe_report_command_state();
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(
    VkCommandBuffer command_buffer, uint32_t first_binding,
    uint32_t binding_count, const VkBuffer* buffers,
    const VkDeviceSize* offsets) {
  static const auto function =
      host_function<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers");
  if (!function) return;
  const bool dedup = command_state_dedup_enabled();
  if (!dedup) {
    function(command_buffer, first_binding, binding_count, buffers, offsets);
    return;
  }
  CommandStateSnapshot snapshot;
  snapshot.kind = CommandStateKind::vertex_buffers;
  snapshot.first_binding = first_binding;
  if (buffers && binding_count)
    snapshot.buffers.assign(buffers, buffers + binding_count);
  if (offsets && binding_count)
    snapshot.offsets.assign(offsets, offsets + binding_count);
  ++command_state_calls;
  if (command_state_is_duplicate(command_buffer, snapshot)) {
    ++command_state_suppressed;
    maybe_report_command_state();
    return;
  }
  function(command_buffer, first_binding, binding_count, buffers, offsets);
  remember_command_state(command_buffer, std::move(snapshot));
  maybe_report_command_state();
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(
    VkCommandBuffer command_buffer, uint32_t first_binding,
    uint32_t binding_count, const VkBuffer* buffers,
    const VkDeviceSize* offsets, const VkDeviceSize* sizes,
    const VkDeviceSize* strides) {
  static const auto function =
      host_function<PFN_vkCmdBindVertexBuffers2>("vkCmdBindVertexBuffers2");
  if (!function) return;
  const bool dedup = command_state_dedup_enabled();
  if (!dedup) {
    function(command_buffer, first_binding, binding_count, buffers, offsets,
             sizes, strides);
    return;
  }
  CommandStateSnapshot snapshot;
  snapshot.kind = CommandStateKind::vertex_buffers2;
  snapshot.first_binding = first_binding;
  snapshot.sizes_present = sizes != nullptr;
  snapshot.strides_present = strides != nullptr;
  if (buffers && binding_count)
    snapshot.buffers.assign(buffers, buffers + binding_count);
  if (offsets && binding_count)
    snapshot.offsets.assign(offsets, offsets + binding_count);
  if (sizes && binding_count)
    snapshot.sizes.assign(sizes, sizes + binding_count);
  if (strides && binding_count)
    snapshot.strides.assign(strides, strides + binding_count);
  ++command_state_calls;
  if (command_state_is_duplicate(command_buffer, snapshot)) {
    ++command_state_suppressed;
    maybe_report_command_state();
    return;
  }
  function(command_buffer, first_binding, binding_count, buffers, offsets,
           sizes, strides);
  remember_command_state(command_buffer, std::move(snapshot));
  maybe_report_command_state();
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(
    VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point,
    VkPipelineLayout layout, uint32_t first_set, uint32_t descriptor_set_count,
    const VkDescriptorSet* descriptor_sets, uint32_t dynamic_offset_count,
    const uint32_t* dynamic_offsets) {
  static const auto function = host_function<PFN_vkCmdBindDescriptorSets>(
      "vkCmdBindDescriptorSets");
  if (!function) return;

  const bool dedup = descriptor_bind_dedup_enabled();
  const bool trace = engine_trace_enabled();
  if (!dedup && !trace) {
    function(command_buffer, bind_point, layout, first_set, descriptor_set_count,
             descriptor_sets, dynamic_offset_count, dynamic_offsets);
    return;
  }

  if (dedup) {
    ++descriptor_bind_calls;
    DescriptorBindState& state = descriptor_bind_state();
    bool duplicate = false;
    {
      std::scoped_lock lock(state.mutex);
      const auto found =
          state.last_bind.find(reinterpret_cast<uintptr_t>(command_buffer));
      if (found != state.last_bind.end()) {
        duplicate = descriptor_bind_matches(
            found->second, bind_point, layout, first_set,
            descriptor_set_count, descriptor_sets, dynamic_offset_count,
            dynamic_offsets);
      }
    }
    if (duplicate) {
      ++descriptor_bind_suppressed;
      maybe_report_descriptor_bind_dedup();
      return;
    }
  }

  const uint64_t started_ns = trace ? monotonic_ns() : 0;
  function(command_buffer, bind_point, layout, first_set, descriptor_set_count,
           descriptor_sets, dynamic_offset_count, dynamic_offsets);
  if (dedup)
    remember_descriptor_bind(command_buffer, bind_point, layout, first_set,
                             descriptor_set_count, descriptor_sets,
                             dynamic_offset_count, dynamic_offsets);
  if (dedup) maybe_report_descriptor_bind_dedup();
  if (trace)
    record_engine_event(EngineEvent::descriptor_bind, monotonic_ns() - started_ns,
                        descriptor_set_count);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer command_buffer, VkPipelineStageFlags source_stage_mask,
    VkPipelineStageFlags destination_stage_mask, VkDependencyFlags dependency_flags,
    uint32_t memory_barrier_count, const VkMemoryBarrier* memory_barriers,
    uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier* buffer_memory_barriers,
    uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier* image_memory_barriers) {
  static const auto function =
      host_function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  if (!function) return;
  const bool trace = engine_trace_enabled();
  if (!trace) {
    function(command_buffer, source_stage_mask, destination_stage_mask,
             dependency_flags, memory_barrier_count, memory_barriers,
             buffer_memory_barrier_count, buffer_memory_barriers,
             image_memory_barrier_count, image_memory_barriers);
    return;
  }
  const uint64_t started_ns = monotonic_ns();
  function(command_buffer, source_stage_mask, destination_stage_mask,
           dependency_flags, memory_barrier_count, memory_barriers,
           buffer_memory_barrier_count, buffer_memory_barriers,
           image_memory_barrier_count, image_memory_barriers);
  record_engine_event(EngineEvent::barrier, monotonic_ns() - started_ns,
                      static_cast<uint64_t>(memory_barrier_count) +
                          buffer_memory_barrier_count + image_memory_barrier_count);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* image_index) {
  static const auto function =
      host_function<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!wait_trace_enabled() && !engine_trace_enabled()) {
    return function(device, swapchain, timeout, semaphore, fence, image_index);
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result =
      function(device, swapchain, timeout, semaphore, fence, image_index);
  const uint64_t elapsed_ns = monotonic_ns() - started_ns;
  record_engine_event(EngineEvent::acquire, elapsed_ns);
  const uint64_t elapsed_us = elapsed_ns / 1000ULL;
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
      (perf_trace_enabled() || engine_trace_enabled() ||
       frame_work_trace_enabled())) {
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
      (wait_trace_enabled() || engine_trace_enabled())) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkWaitForFences);
  }
  if (name && std::strcmp(name, "vkQueueSubmit") == 0 &&
      (submit_trace_enabled() || engine_trace_enabled() ||
       frame_work_trace_enabled()) &&
      host_function<PFN_vkQueueSubmit>("vkQueueSubmit")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueSubmit);
  }
  if (name && std::strcmp(name, "vkQueueSubmit2") == 0 &&
      (submit_trace_enabled() || engine_trace_enabled() ||
       frame_work_trace_enabled()) &&
      host_function<PFN_vkQueueSubmit2>("vkQueueSubmit2")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueSubmit2);
  }
  if (name && std::strcmp(name, "vkCreateImage") == 0 &&
      (engine_trace_enabled() || intel_format_trace_enabled() ||
       texture_upload_trace_enabled() || frame_work_trace_enabled()) &&
      host_function<PFN_vkCreateImage>("vkCreateImage")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImage);
  }
  if (name && std::strcmp(name, "vkDestroyImage") == 0 &&
      (texture_upload_trace_enabled() || frame_work_trace_enabled()) &&
      host_function<PFN_vkDestroyImage>("vkDestroyImage")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImage);
  }
  if (name && std::strcmp(name, "vkBindBufferMemory") == 0 &&
      texture_upload_hash_trace_enabled() &&
      host_function<PFN_vkBindBufferMemory>("vkBindBufferMemory")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkBindBufferMemory);
  }
  if (name && std::strcmp(name, "vkDestroyBuffer") == 0 &&
      texture_upload_hash_trace_enabled() &&
      host_function<PFN_vkDestroyBuffer>("vkDestroyBuffer")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyBuffer);
  }
  if (name && std::strcmp(name, "vkMapMemory") == 0 &&
      texture_upload_hash_trace_enabled() &&
      host_function<PFN_vkMapMemory>("vkMapMemory")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkMapMemory);
  }
  if (name && std::strcmp(name, "vkUnmapMemory") == 0 &&
      texture_upload_hash_trace_enabled() &&
      host_function<PFN_vkUnmapMemory>("vkUnmapMemory")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkUnmapMemory);
  }
  if (name && std::strcmp(name, "vkCmdCopyBufferToImage") == 0 &&
      (copy_trace_enabled() || engine_trace_enabled() ||
       texture_upload_trace_enabled() || frame_work_trace_enabled()) &&
      host_function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyBufferToImage);
  }
  if (name && std::strcmp(name, "vkCmdCopyBufferToImage2") == 0 &&
      (copy_trace_enabled() || engine_trace_enabled() ||
       texture_upload_trace_enabled() || frame_work_trace_enabled()) &&
      host_function<PFN_vkCmdCopyBufferToImage2>("vkCmdCopyBufferToImage2")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyBufferToImage2);
  }
  if (name && std::strcmp(name, "vkCmdCopyImage2") == 0 &&
      (copy_trace_enabled() || engine_trace_enabled()) &&
      host_function<PFN_vkCmdCopyImage2>("vkCmdCopyImage2")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyImage2);
  }
  if (name && std::strcmp(name, "vkCmdCopyImage") == 0 &&
      (copy_trace_enabled() || engine_trace_enabled()) &&
      host_function<PFN_vkCmdCopyImage>("vkCmdCopyImage")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyImage);
  }
  if (name && std::strcmp(name, "vkAcquireNextImageKHR") == 0 &&
      (wait_trace_enabled() || engine_trace_enabled())) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkAcquireNextImageKHR);
  }
  if (name && std::strcmp(name, "vkAllocateDescriptorSets") == 0 &&
      (engine_trace_enabled() || descriptor_alloc_batch_size()) &&
      host_function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateDescriptorSets);
  }
  if (name && std::strcmp(name, "vkFreeDescriptorSets") == 0 &&
      engine_trace_enabled() &&
      host_function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeDescriptorSets);
  }
  if (name && std::strcmp(name, "vkCreateDescriptorPool") == 0 &&
      engine_trace_enabled() &&
      host_function<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDescriptorPool);
  }
  if (name && std::strcmp(name, "vkResetDescriptorPool") == 0 &&
      (engine_trace_enabled() || descriptor_alloc_batch_size()) &&
      host_function<PFN_vkResetDescriptorPool>("vkResetDescriptorPool")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkResetDescriptorPool);
  }
  if (name && std::strcmp(name, "vkDestroyDescriptorPool") == 0 &&
      descriptor_alloc_batch_size() &&
      host_function<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDescriptorPool);
  }
  if (name && std::strcmp(name, "vkUpdateDescriptorSets") == 0 &&
      engine_trace_enabled() &&
      host_function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkUpdateDescriptorSets);
  }
  if (name && std::strcmp(name, "vkCreateImageView") == 0 &&
      engine_trace_enabled() &&
      host_function<PFN_vkCreateImageView>("vkCreateImageView")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImageView);
  }
  if (name && std::strcmp(name, "vkCreateSampler") == 0 &&
      texture_min_lod() > 0.0f &&
      host_function<PFN_vkCreateSampler>("vkCreateSampler")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSampler);
  }
  if (descriptor_bind_dedup_enabled() && name &&
      std::strcmp(name, "vkBeginCommandBuffer") == 0 &&
      host_function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkBeginCommandBuffer);
  }
  if (descriptor_bind_dedup_enabled() && name &&
      std::strcmp(name, "vkEndCommandBuffer") == 0 &&
      host_function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkEndCommandBuffer);
  }
  if (descriptor_bind_dedup_enabled() && name &&
      std::strcmp(name, "vkResetCommandBuffer") == 0 &&
      host_function<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkResetCommandBuffer);
  }
  if (descriptor_bind_dedup_enabled() && name &&
      std::strcmp(name, "vkFreeCommandBuffers") == 0 &&
      host_function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeCommandBuffers);
  }
  if (descriptor_bind_dedup_enabled() && name &&
      std::strcmp(name, "vkCmdExecuteCommands") == 0 &&
      host_function<PFN_vkCmdExecuteCommands>("vkCmdExecuteCommands")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdExecuteCommands);
  }
  if (command_state_dedup_enabled() && name &&
      std::strcmp(name, "vkCmdBindPipeline") == 0 &&
      host_function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindPipeline);
  }
  if (command_state_dedup_enabled() && name &&
      std::strcmp(name, "vkCmdBindIndexBuffer") == 0 &&
      host_function<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindIndexBuffer);
  }
  if (command_state_dedup_enabled() && name &&
      std::strcmp(name, "vkCmdBindVertexBuffers") == 0 &&
      host_function<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindVertexBuffers);
  }
  if (command_state_dedup_enabled() && name &&
      std::strcmp(name, "vkCmdBindVertexBuffers2") == 0 &&
      host_function<PFN_vkCmdBindVertexBuffers2>("vkCmdBindVertexBuffers2")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindVertexBuffers2);
  }
  if (name && std::strcmp(name, "vkCmdBindDescriptorSets") == 0 &&
      (engine_trace_enabled() || descriptor_bind_dedup_enabled()) &&
      host_function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindDescriptorSets);
  }
  if (name && std::strcmp(name, "vkCmdPipelineBarrier") == 0 &&
      engine_trace_enabled() &&
      host_function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdPipelineBarrier);
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
  const bool record_metrics = perf_trace_enabled() || engine_trace_enabled();
  const bool record_frame_work = frame_work_trace_enabled();
  if (!record_metrics && !record_frame_work)
    return function(queue, present_info);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue, present_info);
  const uint64_t finished_ns = monotonic_ns();
  if (record_metrics) record_present(started_ns, finished_ns);
  if (record_frame_work) record_frame_present(finished_ns);
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
  if (std::strcmp(name, "vkCmdCopyImage2") == 0 &&
      (copy_trace_enabled() || engine_trace_enabled()) &&
      host_function<PFN_vkCmdCopyImage2>("vkCmdCopyImage2")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyImage2);
  }
  if (std::strcmp(name, "vkCmdCopyImage") == 0 &&
      (copy_trace_enabled() || engine_trace_enabled()) &&
      host_function<PFN_vkCmdCopyImage>("vkCmdCopyImage")) {
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdCopyImage);
  }
  if (descriptor_bind_dedup_enabled()) {
    if (std::strcmp(name, "vkBeginCommandBuffer") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkBeginCommandBuffer);
    if (std::strcmp(name, "vkEndCommandBuffer") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkEndCommandBuffer);
    if (std::strcmp(name, "vkResetCommandBuffer") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkResetCommandBuffer);
    if (std::strcmp(name, "vkFreeCommandBuffers") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeCommandBuffers);
    if (std::strcmp(name, "vkCmdExecuteCommands") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdExecuteCommands);
    if (std::strcmp(name, "vkCmdBindDescriptorSets") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindDescriptorSets);
  }
  if (descriptor_alloc_batch_size()) {
    if (std::strcmp(name, "vkAllocateDescriptorSets") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateDescriptorSets);
    if (std::strcmp(name, "vkResetDescriptorPool") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkResetDescriptorPool);
    if (std::strcmp(name, "vkDestroyDescriptorPool") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyDescriptorPool);
  }
  if (command_state_dedup_enabled()) {
    if (std::strcmp(name, "vkCmdBindPipeline") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindPipeline);
    if (std::strcmp(name, "vkCmdBindIndexBuffer") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindIndexBuffer);
    if (std::strcmp(name, "vkCmdBindVertexBuffers") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindVertexBuffers);
    if (std::strcmp(name, "vkCmdBindVertexBuffers2") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindVertexBuffers2);
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
