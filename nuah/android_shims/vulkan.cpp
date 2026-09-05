#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <dlfcn.h>
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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "nuah/android_abi_registry.h"
#include "nuah/native_window_bridge.h"
#ifndef NUAH_LIKELY
#define NUAH_LIKELY(x) (__builtin_expect(!!(x), 1))
#endif
#ifndef NUAH_UNLIKELY
#define NUAH_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif
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
    if (std::strcmp(raw, "fifo_relaxed") == 0) return RequestedPresentMode::fifo_relaxed;
    if (std::strcmp(raw, "mailbox") == 0) return RequestedPresentMode::mailbox;
    if (std::strcmp(raw, "immediate") == 0) return RequestedPresentMode::immediate;
    return RequestedPresentMode::mailbox;
  }();
  return value;
}
struct PresentModeEntry {
  RequestedPresentMode mode;
  VkPresentModeKHR value;
  const char* name;
};
constexpr PresentModeEntry kPresentModes[] = {{RequestedPresentMode::fifo, VK_PRESENT_MODE_FIFO_KHR, "fifo"}, {RequestedPresentMode::fifo_relaxed, VK_PRESENT_MODE_FIFO_RELAXED_KHR, "fifo_relaxed"},
                                               {RequestedPresentMode::mailbox, VK_PRESENT_MODE_MAILBOX_KHR, "mailbox"}, {RequestedPresentMode::immediate, VK_PRESENT_MODE_IMMEDIATE_KHR, "immediate"}};
// One table drives value + name lookups (same mapping, no behavior change).
inline const PresentModeEntry* find_present_entry(RequestedPresentMode want) {
  for (const auto& e : kPresentModes)
    if (e.mode == want) return &e;
  return nullptr;
}
VkPresentModeKHR requested_present_mode_value() {
  const auto* e = find_present_entry(requested_present_mode());
  return e ? e->value : VK_PRESENT_MODE_MAX_ENUM_KHR;
}
const char* requested_present_mode_name() {
  const auto* e = find_present_entry(requested_present_mode());
  return e ? e->name : "unknown";
}
uint32_t find_present_mode_index(const VkPresentModeKHR* modes, uint32_t count, VkPresentModeKHR want) {
  for (uint32_t i = 0; i < count; ++i)
    if (modes[i] == want) return i;
  return count;
}
#define DEFINE_ENV_FLAG(func_name, env_name)      \
  bool func_name() {                              \
    static const bool val = [] {                  \
      const char* r = std::getenv(env_name);      \
      return r && *r && std::strcmp(r, "0") != 0; \
    }();                                          \
    return val;                                   \
  }
DEFINE_ENV_FLAG(perf_trace_enabled, "NUAH_PERF_TRACE")
DEFINE_ENV_FLAG(bootstrap_trace_enabled, "NUAH_BOOTSTRAP_TRACE")
DEFINE_ENV_FLAG(surface_size_locked, "NUAH_LOCK_SURFACE_SIZE")
// Compact 1 Hz renderer-workload trace; Intel-format trace stays separate (evidence before conversion).
DEFINE_ENV_FLAG(engine_trace_enabled, "NUAH_ENGINE_TRACE")
DEFINE_ENV_FLAG(intel_format_trace_enabled, "NUAH_INTEL_FORMAT_TRACE")
DEFINE_ENV_FLAG(texture_upload_trace_enabled, "NUAH_TEXTURE_UPLOAD_TRACE")
DEFINE_ENV_FLAG(texture_upload_hash_trace_enabled, "NUAH_TEXTURE_UPLOAD_HASH_TRACE")
DEFINE_ENV_FLAG(texture_upload_dedup_enabled, "NUAH_TEXTURE_UPLOAD_DEDUP")
// ANV Gen9 descriptor path rebuilds on redundant binds; opt-in exact-repeat suppression only.
DEFINE_ENV_FLAG(descriptor_bind_dedup_enabled, "NUAH_DESCRIPTOR_BIND_DEDUP")
DEFINE_ENV_FLAG(descriptor_bind_dedup_trace_enabled, "NUAH_DESCRIPTOR_BIND_DEDUP_TRACE")
// Pre-allocate same-layout sets from the app pool (trades capacity for fewer allocator calls).
uint32_t descriptor_alloc_batch_size() {
  static const uint32_t value = [] {
    const char* raw = std::getenv("NUAH_DESCRIPTOR_ALLOC_BATCH");
    if (!raw || !*raw) return 4U;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed > 16) return 0U;
    return static_cast<uint32_t>(parsed);
  }();
  return value;
}
DEFINE_ENV_FLAG(descriptor_alloc_trace_enabled, "NUAH_DESCRIPTOR_ALLOC_TRACE")
// Per-present attribution: submission is evidence of GPU work, not proof of new pixels.
DEFINE_ENV_FLAG(frame_work_trace_enabled, "NUAH_FRAME_WORK_TRACE")
bool upload_fingerprint_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_UPLOAD_FINGERPRINT");
    return raw && *raw && std::strcmp(raw, "0") != 0;
  }();
  return value;
}
void gcc_upload_fingerprint(const uint8_t* bytes, int32_t length, uint64_t* result) {
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
// Opt-in sampler-policy mip clamp (not a fake caps report); UI samplers without mips unchanged.
float texture_min_lod() {
  static const float value = [] {
    const char* raw = std::getenv("NUAH_TEXTURE_MIN_LOD");
    if (!raw || !*raw) return 0.0f;
    char* end = nullptr;
    const float parsed = std::strtof(raw, &end);
    if (end == raw || *end != '\0' || parsed < 0.0f || parsed > 4.0f) return 0.0f;
    return parsed;
  }();
  return value;
}
DEFINE_ENV_FLAG(texture_min_lod_trace_enabled, "NUAH_TEXTURE_MIN_LOD_TRACE")
uint64_t monotonic_ns() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
// Shared 1 Hz gate for rate-limited diagnostics. Single attempt so concurrent
// reporters never spin; at most one emitter wins per second.
inline bool throttle_1hz(std::atomic<uint64_t>& next_ns, uint64_t now) {
  uint64_t next = next_ns.load(std::memory_order_relaxed);
  if (now < next) return false;
  return next_ns.compare_exchange_strong(next, now + 1000000000ULL, std::memory_order_relaxed, std::memory_order_relaxed);
}
/* Per-copy printf is itself capable of blocking the render thread while a
 * texture mip chain is recorded.  Keep the diagnostic opt-in, but rate-limit
 * it so enabling the trace cannot manufacture a frame hitch. */
bool copy_trace_log_slot() {
  static std::atomic<uint64_t> next_log_ns{0};
  return throttle_1hz(next_log_ns, monotonic_ns());
}
template <typename T>
inline void assign_vec(std::vector<T>& dst, const T* src, uint32_t n) {
  dst.clear();
  if (src && n) dst.assign(src, src + n);
}
enum class EngineEvent : std::size_t { image_create, copy_to_image, descriptor_allocate, descriptor_update, descriptor_free, descriptor_pool_create, descriptor_pool_reset, descriptor_bind, image_view_create, barrier, submit, wait_fence, acquire, count };
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
  bool operator==(const DescriptorPoolLayoutKey& other) const noexcept { return pool == other.pool && layout == other.layout; }
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
  std::unordered_map<DescriptorPoolLayoutKey, std::vector<VkDescriptorSet>, DescriptorPoolLayoutHash> sets;
};
DescriptorAllocCache& descriptor_alloc_cache() {
  static DescriptorAllocCache value;
  return value;
}
std::atomic<uint64_t> descriptor_alloc_calls{0}, descriptor_alloc_driver_calls{0}, descriptor_alloc_driver_sets{0}, descriptor_alloc_cache_hits{0}, descriptor_alloc_batched_sets{0};
bool take_cached_descriptor_set(VkDescriptorPool pool, VkDescriptorSetLayout layout, VkDescriptorSet* output) {
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  const auto it = cache.sets.find({pool, layout});
  if (it == cache.sets.end() || it->second.empty()) return false;
  *output = it->second.back();
  it->second.pop_back();
  return true;
}
void restore_cached_descriptor_sets(VkDescriptorPool pool, const std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>>& sets) {
  if (sets.empty()) return;
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  for (const auto& [layout, descriptor_set] : sets) cache.sets[{pool, layout}].push_back(descriptor_set);
}
std::vector<VkDescriptorSet> retain_batched_descriptor_sets(VkDescriptorPool pool, const std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>>& sets) {
  constexpr std::size_t kPerLayoutLimit = 32;
  std::vector<VkDescriptorSet> discard;
  if (sets.empty()) return discard;
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  for (const auto& [layout, descriptor_set] : sets) {
    auto& retained = cache.sets[{pool, layout}];
    if (retained.size() < kPerLayoutLimit) retained.push_back(descriptor_set);
    else discard.push_back(descriptor_set);
  }
  return discard;
}
void clear_cached_descriptor_pool(VkDescriptorPool pool) {
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  for (auto it = cache.sets.begin(); it != cache.sets.end();) {
    if (it->first.pool == pool) it = cache.sets.erase(it);
    else ++it;
  }
}
void clear_all_cached_descriptor_sets() {
  DescriptorAllocCache& cache = descriptor_alloc_cache();
  std::scoped_lock lock(cache.mutex);
  cache.sets.clear();
}
// Shared 1 Hz report gate: one static slot per call site, single winner per second.
inline bool report_slot_1hz(bool enabled, std::atomic<uint64_t>& slot) {
  if (!enabled) return false;
  return throttle_1hz(slot, monotonic_ns());
}
void maybe_report_descriptor_alloc() {
  static std::atomic<uint64_t> next_report_ns{0};
  if (!report_slot_1hz(descriptor_alloc_trace_enabled(), next_report_ns)) return;
  std::fprintf(stderr, "nuah descriptor-alloc: requests=%llu driver_calls=%llu driver_sets=%llu cache_hits=%llu batched_sets=%llu\n",
                static_cast<unsigned long long>(descriptor_alloc_calls.load(std::memory_order_relaxed)), static_cast<unsigned long long>(descriptor_alloc_driver_calls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(descriptor_alloc_driver_sets.load(std::memory_order_relaxed)), static_cast<unsigned long long>(descriptor_alloc_cache_hits.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(descriptor_alloc_batched_sets.load(std::memory_order_relaxed)));
}
DEFINE_ENV_FLAG(command_state_dedup_enabled, "NUAH_COMMAND_STATE_DEDUP")
DEFINE_ENV_FLAG(command_state_dedup_trace_enabled, "NUAH_COMMAND_STATE_DEDUP_TRACE")
enum class CommandStateKind : uint8_t { pipeline, index_buffer, vertex_buffers, vertex_buffers2 };
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
  // One snapshot per kind: interleaved pipeline/index/vertex binds still hit exact repeats.
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
bool command_state_matches(const CommandStateSnapshot& previous, const CommandStateSnapshot& current) {
  return previous.valid && previous.kind == current.kind && previous.bind_point == current.bind_point && previous.pipeline == current.pipeline && previous.index_buffer == current.index_buffer &&
         previous.index_offset == current.index_offset && previous.index_type == current.index_type && previous.first_binding == current.first_binding && previous.sizes_present == current.sizes_present &&
         previous.strides_present == current.strides_present && previous.buffers == current.buffers && previous.offsets == current.offsets && previous.sizes == current.sizes && previous.strides == current.strides;
}
bool command_state_is_duplicate(VkCommandBuffer command_buffer, const CommandStateSnapshot& current) {
  CommandStateCache& cache = command_state_cache();
  std::scoped_lock lock(cache.mutex);
  const auto iterator = cache.last_state.find(reinterpret_cast<uintptr_t>(command_buffer));
  if (iterator == cache.last_state.end()) return false;
  const std::size_t index = static_cast<std::size_t>(current.kind);
  if (index >= iterator->second.state.size()) return false;
  return command_state_matches(iterator->second.state[index], current);
}
void remember_command_state(VkCommandBuffer command_buffer, CommandStateSnapshot snapshot) {
  snapshot.valid = true;
  CommandStateCache& cache = command_state_cache();
  std::scoped_lock lock(cache.mutex);
  const std::size_t index = static_cast<std::size_t>(snapshot.kind);
  auto& slot = cache.last_state[reinterpret_cast<uintptr_t>(command_buffer)];
  if (index >= slot.state.size()) return;
  slot.state[index] = std::move(snapshot);
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
  static std::atomic<uint64_t> next_report_ns{0};
  if (!report_slot_1hz(command_state_dedup_trace_enabled(), next_report_ns)) return;
  std::fprintf(stderr, "nuah command-state: calls=%llu suppressed=%llu\n", static_cast<unsigned long long>(command_state_calls.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(command_state_suppressed.load(std::memory_order_relaxed)));
}
DescriptorBindState& descriptor_bind_state() {
  static DescriptorBindState value;
  return value;
}
std::atomic<uint64_t> descriptor_bind_calls{0};
std::atomic<uint64_t> descriptor_bind_suppressed{0};
bool descriptor_bind_matches(const DescriptorBindSnapshot& previous, VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set, uint32_t descriptor_set_count,
                             const VkDescriptorSet* descriptor_sets, uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets) {
  if (!previous.valid || previous.bind_point != bind_point || previous.layout != layout || previous.first_set != first_set || previous.descriptor_sets.size() != descriptor_set_count ||
      previous.dynamic_offsets.size() != dynamic_offset_count)
    return false;
  if (descriptor_set_count && std::memcmp(previous.descriptor_sets.data(), descriptor_sets, sizeof(VkDescriptorSet) * descriptor_set_count) != 0) return false;
  if (dynamic_offset_count && std::memcmp(previous.dynamic_offsets.data(), dynamic_offsets, sizeof(uint32_t) * dynamic_offset_count) != 0) return false;
  return true;
}
void remember_descriptor_bind(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set, uint32_t descriptor_set_count,
                              const VkDescriptorSet* descriptor_sets, uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets) {
  DescriptorBindState& state = descriptor_bind_state();
  std::scoped_lock lock(state.mutex);
  DescriptorBindSnapshot& snapshot = state.last_bind[reinterpret_cast<uintptr_t>(command_buffer)];
  snapshot.bind_point = bind_point;
  snapshot.layout = layout;
  snapshot.first_set = first_set;
  assign_vec(snapshot.descriptor_sets, descriptor_sets, descriptor_set_count);
  assign_vec(snapshot.dynamic_offsets, dynamic_offsets, dynamic_offset_count);
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
  static std::atomic<uint64_t> next_report_ns{0};
  if (!report_slot_1hz(descriptor_bind_dedup_trace_enabled(), next_report_ns)) return;
  std::fprintf(stderr, "nuah descriptor-dedup: binds=%llu suppressed=%llu\n", static_cast<unsigned long long>(descriptor_bind_calls.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(descriptor_bind_suppressed.load(std::memory_order_relaxed)));
}
struct UploadSubresource {
  uint64_t image_id = 0;
  uint32_t mip_level = 0, layer = 0;
  VkImageAspectFlags aspect = 0;
  int32_t x = 0, y = 0, z = 0;
  uint32_t width = 0, height = 0, depth = 0;
  bool operator==(const UploadSubresource& other) const = default;
};
struct UploadSubresourceHash {
  std::size_t operator()(const UploadSubresource& value) const {
    std::size_t result = std::hash<uint64_t>{}(value.image_id);
    const auto combine = [&result](uint64_t part) { result ^= std::hash<uint64_t>{}(part) + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2); };
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
  uint64_t regions = 0, bytes = 0, repeated_regions = 0, repeated_bytes = 0, hashed_regions = 0, identical_regions = 0, identical_bytes = 0, suppressed_regions = 0, suppressed_bytes = 0, fingerprint_calls = 0, fingerprint_ns = 0;
};
struct PendingFrameWork {
  uint64_t submit_calls = 0, submit_batches = 0, command_buffers = 0, requested_copy_regions = 0, forwarded_copy_regions = 0, forwarded_copy_bytes = 0;
};
struct FrameWorkWindow {
  uint64_t presents = 0, presents_with_submit = 0, presents_without_submit = 0, submit_calls = 0, submit_batches = 0, command_buffers = 0, requested_copy_regions = 0, forwarded_copy_regions = 0, forwarded_copy_bytes = 0, max_command_buffers_per_present = 0, max_forwarded_copy_bytes_per_present = 0;
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
  std::array<EngineAggregate, static_cast<std::size_t>(EngineEvent::count)> events{};
  std::unordered_map<VkFormat, ImageFormatAggregate> image_formats;
  std::unordered_map<uintptr_t, TrackedImage> images;
  std::unordered_map<uintptr_t, TrackedBuffer> buffers;
  std::unordered_map<uintptr_t, MappedMemory> mapped_memory;
  std::unordered_map<UploadSubresource, uint64_t, UploadSubresourceHash> seen_uploads;
  UploadAggregate uploads;
  uint64_t next_image_id = 1;
  uint64_t next_report_ns = 0;
};
EngineTrace& engine_trace() {
  static EngineTrace value;
  return value;
}
void record_engine_event(EngineEvent event, uint64_t elapsed_ns, uint64_t units = 1);
// Single lock entry for cold trace bookkeeping; hot paths early-out before calling.
template <typename Fn>
inline void with_engine_trace(Fn&& fn) {
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  fn(trace);
}
inline UploadSubresource make_upload_key(uint64_t image_id, const VkBufferImageCopy& region) {
  return {image_id, region.imageSubresource.mipLevel, region.imageSubresource.baseArrayLayer, region.imageSubresource.aspectMask, region.imageOffset.x, region.imageOffset.y,
          region.imageOffset.z, region.imageExtent.width, region.imageExtent.height, region.imageExtent.depth};
}
// Zero-alloc fast path when tracing is off; timed forward otherwise.
template <typename Fn>
inline VkResult traced_result(EngineEvent event, uint64_t units, Fn&& fn) {
  if (NUAH_LIKELY(!engine_trace_enabled())) return fn();
  const uint64_t start = monotonic_ns();
  const VkResult result = fn();
  record_engine_event(event, monotonic_ns() - start, units);
  return result;
}
template <typename Fn>
inline void traced_void(EngineEvent event, uint64_t units, Fn&& fn) {
  if (NUAH_LIKELY(!engine_trace_enabled())) {
    fn();
    return;
  }
  const uint64_t start = monotonic_ns();
  fn();
  record_engine_event(event, monotonic_ns() - start, units);
}
// Invalidate per-command-buffer dedup caches (Begin/End/Execute paths).
inline void clear_cmd_caches(VkCommandBuffer cb) {
  if (descriptor_bind_dedup_enabled()) clear_descriptor_bind(cb);
  if (command_state_dedup_enabled()) clear_command_state(cb);
}
inline void clear_cmd_caches_on_success(VkCommandBuffer cb, VkResult result) {
  if (result == VK_SUCCESS) clear_cmd_caches(cb);
}
// Unified vk* forwarder codegen: resolve + trace-gate + forward + bookkeeping.
// Headers stay explicit so `grep ^VKAPI_ATTR.*VKAPI_CALL vk` still lists every
// export; bodies collapse to one macro call. Hot paths early-out before any
// alloc/timing (zero-alloc when tracing off). Static cache avoids per-call dlsym.
#define NUAH_VK_HOST(PFN_T, FN, ...) \
  static const auto nuah_host_fn = host_function<PFN_T>(#FN); \
  if (NUAH_UNLIKELY(!nuah_host_fn)) return __VA_ARGS__
#define NUAH_VK_TRACED_RESULT(PFN_T, FN, EVENT, UNITS, ...) \
  NUAH_VK_HOST(PFN_T, FN, VK_ERROR_INITIALIZATION_FAILED); \
  return traced_result(EVENT, UNITS, [&] { return nuah_host_fn(__VA_ARGS__); })
#define NUAH_VK_TRACED_VOID(PFN_T, FN, EVENT, UNITS, ...) \
  NUAH_VK_HOST(PFN_T, FN); \
  return traced_void(EVENT, UNITS, [&] { nuah_host_fn(__VA_ARGS__); })
#define NUAH_VK_CMD_RESULT(PFN_T, FN, AFTER, ...) \
  NUAH_VK_HOST(PFN_T, FN, VK_ERROR_INITIALIZATION_FAILED); \
  const VkResult nuah_result = nuah_host_fn(__VA_ARGS__); \
  AFTER; \
  return nuah_result
#define NUAH_VK_CMD_VOID(PFN_T, FN, AFTER, ...) \
  NUAH_VK_HOST(PFN_T, FN); \
  nuah_host_fn(__VA_ARGS__); \
  AFTER
// Cold tracking paths keep per-call resolve (matches original, no static cache).
#define NUAH_VK_TRACKED_RESULT(PFN_T, FN, ERR, ON_SUCCESS, ...) \
  const auto nuah_host_fn = host_function<PFN_T>(#FN); \
  if (!nuah_host_fn) return ERR; \
  const VkResult nuah_result = nuah_host_fn(__VA_ARGS__); \
  if (nuah_result == VK_SUCCESS) { ON_SUCCESS; } \
  return nuah_result
#define NUAH_VK_FORGET_VOID(PFN_T, FN, ON_ENTRY, ...) \
  ON_ENTRY; \
  const auto nuah_host_fn = host_function<PFN_T>(#FN); \
  if (nuah_host_fn) nuah_host_fn(__VA_ARGS__)
#define NUAH_VK_COPY_IMAGE(PFN_T, FN, NAME, COUNT, REGIONS, ...) \
  NUAH_VK_HOST(PFN_T, FN); \
  if (NUAH_LIKELY(!copy_trace_enabled() && !engine_trace_enabled())) { \
    nuah_host_fn(__VA_ARGS__); \
    return; \
  } \
  const uint64_t nuah_started_ns = monotonic_ns(); \
  nuah_host_fn(__VA_ARGS__); \
  trace_image_copy_perf(NAME, nuah_started_ns, COUNT, REGIONS)
#define NUAH_VK_SURFACE_RESULT(PFN_T, FN, CAPS, SURFACE, EXTRA_NULL, ...) \
  const auto nuah_host_fn = host_function<PFN_T>(#FN); \
  if (!nuah_host_fn || (EXTRA_NULL) || !(CAPS)) return VK_ERROR_INITIALIZATION_FAILED; \
  return normalize_result(nuah_host_fn(__VA_ARGS__), CAPS, SURFACE)
// Shared command-state bind forwarder: one list drives all vkCmdBind* dedup wrappers.
// Zero-alloc direct forward when dedup is off; exact-repeat suppression otherwise.
#define NUAH_VK_BIND_STATE(PFN_T, FN, CB, SNAPSHOT, ...) \
  NUAH_VK_HOST(PFN_T, FN); \
  if (NUAH_LIKELY(!command_state_dedup_enabled())) { \
    nuah_host_fn(CB, __VA_ARGS__); \
    return; \
  } \
  apply_command_state(CB, SNAPSHOT, nuah_host_fn, CB, __VA_ARGS__)
inline CommandStateSnapshot make_pipeline_snapshot(VkPipelineBindPoint bp, VkPipeline p) {
  CommandStateSnapshot s;
  s.kind = CommandStateKind::pipeline;
  s.bind_point = bp;
  s.pipeline = p;
  return s;
}
inline CommandStateSnapshot make_index_snapshot(VkBuffer b, VkDeviceSize o, VkIndexType t) {
  CommandStateSnapshot s;
  s.kind = CommandStateKind::index_buffer;
  s.index_buffer = b;
  s.index_offset = o;
  s.index_type = t;
  return s;
}
inline CommandStateSnapshot make_vertex_snapshot(uint32_t first, const VkBuffer* bufs, const VkDeviceSize* offs, uint32_t n) {
  CommandStateSnapshot s;
  s.kind = CommandStateKind::vertex_buffers;
  s.first_binding = first;
  assign_vec(s.buffers, bufs, n);
  assign_vec(s.offsets, offs, n);
  return s;
}
inline CommandStateSnapshot make_vertex2_snapshot(uint32_t first, const VkBuffer* bufs, const VkDeviceSize* offs, const VkDeviceSize* sizes, const VkDeviceSize* strides, uint32_t n) {
  CommandStateSnapshot s;
  s.kind = CommandStateKind::vertex_buffers2;
  s.first_binding = first;
  s.sizes_present = sizes != nullptr;
  s.strides_present = strides != nullptr;
  assign_vec(s.buffers, bufs, n);
  assign_vec(s.offsets, offs, n);
  assign_vec(s.sizes, sizes, n);
  assign_vec(s.strides, strides, n);
  return s;
}
// Shared name-table lookup drives engine-event + format names (one helper, two tables).
template <typename Entry, typename Key, typename Get>
inline const char* lookup_table_name(const Entry* table, std::size_t n, Key key, Get get, const char* dflt) {
  for (std::size_t i = 0; i < n; ++i)
    if (get(table[i]) == key) return table[i].name;
  return dflt;
}
struct EngineEventNameEntry {
  EngineEvent event;
  const char* name;
};
constexpr EngineEventNameEntry kEngineEventNames[] = {{EngineEvent::image_create, "image_create"}, {EngineEvent::copy_to_image, "copy_to_image"}, {EngineEvent::descriptor_allocate, "descriptor_allocate"},
                                                       {EngineEvent::descriptor_update, "descriptor_update"}, {EngineEvent::descriptor_free, "descriptor_free"}, {EngineEvent::descriptor_pool_create, "descriptor_pool_create"},
                                                       {EngineEvent::descriptor_pool_reset, "descriptor_pool_reset"}, {EngineEvent::descriptor_bind, "descriptor_bind"}, {EngineEvent::image_view_create, "image_view_create"},
                                                       {EngineEvent::barrier, "barrier"}, {EngineEvent::submit, "submit"}, {EngineEvent::wait_fence, "wait_fence"}, {EngineEvent::acquire, "acquire"}};
const char* engine_event_name(EngineEvent event) {
  return lookup_table_name(kEngineEventNames, sizeof(kEngineEventNames) / sizeof(kEngineEventNames[0]), event, [](const EngineEventNameEntry& e) { return e.event; }, "unknown");
}
struct FormatNameEntry {
  VkFormat format;
  const char* name;
};
constexpr FormatNameEntry kFormatNames[] = {{VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"}, {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8_SRGB"}, {VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1_RGB"}, {VK_FORMAT_BC1_RGB_SRGB_BLOCK, "BC1_RGB_SRGB"},
                                            {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA"}, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_RGBA_SRGB"}, {VK_FORMAT_BC2_UNORM_BLOCK, "BC2"}, {VK_FORMAT_BC2_SRGB_BLOCK, "BC2_SRGB"},
                                            {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"}, {VK_FORMAT_BC3_SRGB_BLOCK, "BC3_SRGB"}, {VK_FORMAT_BC4_UNORM_BLOCK, "BC4"}, {VK_FORMAT_BC4_SNORM_BLOCK, "BC4_SNORM"},
                                            {VK_FORMAT_BC5_UNORM_BLOCK, "BC5"}, {VK_FORMAT_BC5_SNORM_BLOCK, "BC5_SNORM"}, {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_UFLOAT"}, {VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H_SFLOAT"},
                                            {VK_FORMAT_BC7_UNORM_BLOCK, "BC7"}, {VK_FORMAT_BC7_SRGB_BLOCK, "BC7_SRGB"}, {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, "ETC2_RGB"}, {VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, "ETC2_RGB_SRGB"},
                                            {VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, "ETC2_RGBA1"}, {VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, "ETC2_RGBA1_SRGB"}, {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_RGBA8"},
                                            {VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, "ETC2_RGBA8_SRGB"}};
const char* format_name(VkFormat format) {
  return lookup_table_name(kFormatNames, sizeof(kFormatNames) / sizeof(kFormatNames[0]), format, [](const FormatNameEntry& e) { return e.format; }, "other");
}
uint32_t format_block_bytes(VkFormat format) {
  switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return 16;
    default: return 0;
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
    if (compressed_block_bytes) total += ((width + 3) / 4) * ((height + 3) / 4) * depth * compressed_block_bytes * info.arrayLayers;
    else if (info.format == VK_FORMAT_R8G8B8A8_UNORM || info.format == VK_FORMAT_R8G8B8A8_SRGB) total += width * height * depth * 4 * info.arrayLayers;
  }
  return total;
}
uint64_t estimate_region_bytes(VkFormat format, VkExtent3D extent) {
  const uint32_t block_bytes = format_block_bytes(format);
  if (block_bytes) return ((static_cast<uint64_t>(extent.width) + 3) / 4) * ((static_cast<uint64_t>(extent.height) + 3) / 4) * extent.depth * block_bytes;
  const uint64_t texels = static_cast<uint64_t>(extent.width) * extent.height * extent.depth;
  if (format == VK_FORMAT_R8_UNORM) return texels;
  if (format == VK_FORMAT_R8G8_UNORM) return texels * 2;
  if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB) return texels * 4;
  return 0;
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
  aggregate.texels += static_cast<uint64_t>(info->extent.width) * info->extent.height * info->extent.depth * info->arrayLayers;
  aggregate.estimated_bytes += estimate_image_bytes(*info);
}
void record_tracked_image(VkImage image, const VkImageCreateInfo* info) {
  if ((!texture_upload_trace_enabled() && !frame_work_trace_enabled()) || image == VK_NULL_HANDLE || !info) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const VkImageUsageFlags forbidden = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  // Sampled+transfer-dst blit sources stay eligible; graphics/compute-writable images are excluded.
  const bool eligible = (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && (info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && !(info->usage & forbidden);
  trace.images[reinterpret_cast<uintptr_t>(image)] = TrackedImage{trace.next_image_id++, info->format, info->usage, eligible};
}
uint64_t estimate_frame_copy_bytes(VkImage image, uint32_t region_count, const VkBufferImageCopy* regions) {
  if (!frame_work_trace_enabled() || !regions) return 0;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const auto image_it = trace.images.find(reinterpret_cast<uintptr_t>(image));
  if (image_it == trace.images.end()) return 0;
  uint64_t bytes = 0;
  for (uint32_t index = 0; index < region_count; ++index) bytes += estimate_region_bytes(image_it->second.format, regions[index].imageExtent) * regions[index].imageSubresource.layerCount;
  return bytes;
}
// Shared frame-metrics lock entry (cold path only; hot paths early-out before calling).
template <typename Fn>
inline void with_frame_metrics(Fn&& fn) {
  FrameWorkMetrics& metrics = frame_work_metrics();
  std::scoped_lock lock(metrics.mutex);
  fn(metrics);
}
void record_frame_submit(uint32_t batches, uint64_t command_buffers) {
  if (!frame_work_trace_enabled()) return;
  with_frame_metrics([&](FrameWorkMetrics& m) {
    ++m.pending.submit_calls;
    m.pending.submit_batches += batches;
    m.pending.command_buffers += command_buffers;
  });
}
void record_frame_copy(uint32_t requested_regions, uint32_t forwarded_regions, uint64_t forwarded_bytes) {
  if (!frame_work_trace_enabled()) return;
  with_frame_metrics([&](FrameWorkMetrics& m) {
    m.pending.requested_copy_regions += requested_regions;
    m.pending.forwarded_copy_regions += forwarded_regions;
    m.pending.forwarded_copy_bytes += forwarded_bytes;
  });
}
void record_frame_present(uint64_t now) {
  if (!frame_work_trace_enabled()) return;
  FrameWorkMetrics& metrics = frame_work_metrics();
  FrameWorkWindow report{};
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
    metrics.window.max_command_buffers_per_present = std::max(metrics.window.max_command_buffers_per_present, pending.command_buffers);
    metrics.window.max_forwarded_copy_bytes_per_present = std::max(metrics.window.max_forwarded_copy_bytes_per_present, pending.forwarded_copy_bytes);
    if (!metrics.next_report_ns) metrics.next_report_ns = now + 1000000000ULL;
    if (now < metrics.next_report_ns) return;
    report = metrics.window;
    metrics.window = {};
    metrics.next_report_ns = now + 1000000000ULL;
  }
  std::fprintf(stderr, "nuah frame-work: presents=%llu with_submit=%llu without_submit=%llu submit_calls=%llu batches=%llu command_buffers=%llu command_buffers_per_present=%llu copy_regions_requested=%llu copy_regions_forwarded=%llu copy_mib_forwarded=%llu max_command_buffers_per_present=%llu max_copy_mib_per_present=%llu\n",
               static_cast<unsigned long long>(report.presents), static_cast<unsigned long long>(report.presents_with_submit), static_cast<unsigned long long>(report.presents_without_submit),
               static_cast<unsigned long long>(report.submit_calls), static_cast<unsigned long long>(report.submit_batches), static_cast<unsigned long long>(report.command_buffers),
               static_cast<unsigned long long>(report.presents ? report.command_buffers / report.presents : 0), static_cast<unsigned long long>(report.requested_copy_regions),
               static_cast<unsigned long long>(report.forwarded_copy_regions), static_cast<unsigned long long>(report.forwarded_copy_bytes / (1024ULL * 1024ULL)),
               static_cast<unsigned long long>(report.max_command_buffers_per_present), static_cast<unsigned long long>(report.max_forwarded_copy_bytes_per_present / (1024ULL * 1024ULL)));
}
uint64_t source_hash(EngineTrace& trace, VkBuffer buffer, const VkBufferImageCopy& region, uint64_t bytes);
bool should_suppress_upload(VkImage image, const VkBufferImageCopy& region, uint64_t content_hash) {
  if (!texture_upload_dedup_enabled() || !texture_upload_hash_trace_enabled()) return false;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const auto image_it = trace.images.find(reinterpret_cast<uintptr_t>(image));
  if (image_it == trace.images.end() || !image_it->second.dedup_eligible) return false;
  const uint64_t bytes = estimate_region_bytes(image_it->second.format, region.imageExtent) * region.imageSubresource.layerCount;
  if (!content_hash) return false;
  const UploadSubresource key = make_upload_key(image_it->second.id, region);
  const auto existing = trace.seen_uploads.find(key);
  if (existing == trace.seen_uploads.end() || existing->second != content_hash) return false;
  ++trace.uploads.suppressed_regions;
  trace.uploads.suppressed_bytes += bytes;
  return true;
}
std::vector<uint64_t> collect_upload_hashes(VkBuffer source, VkImage image, uint32_t region_count, const VkBufferImageCopy* regions) {
  std::vector<uint64_t> result;
  if (!texture_upload_hash_trace_enabled() || !regions || !region_count) return result;
  result.resize(region_count);
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const auto image_it = trace.images.find(reinterpret_cast<uintptr_t>(image));
  if (image_it == trace.images.end()) return result;
  for (uint32_t index = 0; index < region_count; ++index) {
    // Skip cold uploads: first occurrence cannot be suppressed, hashing only adds render-path CPU.
    const VkBufferImageCopy& region = regions[index];
    const UploadSubresource key = make_upload_key(image_it->second.id, region);
    if (trace.seen_uploads.find(key) == trace.seen_uploads.end()) continue;
    const uint64_t bytes = estimate_region_bytes(image_it->second.format, region.imageExtent) * region.imageSubresource.layerCount;
    result[index] = source_hash(trace, source, regions[index], bytes);
  }
  return result;
}
void forget_tracked_image(VkImage image) {
  if (!texture_upload_trace_enabled() || image == VK_NULL_HANDLE) return;
  with_engine_trace([&](EngineTrace& t) { t.images.erase(reinterpret_cast<uintptr_t>(image)); });
}
uint64_t hash_upload_bytes(const std::byte* bytes, uint64_t size) {
  if (upload_fingerprint_enabled() && size <= 0x7fff'ffffULL) {
    std::array<uint64_t, 2> fingerprint{};
    gcc_upload_fingerprint(reinterpret_cast<const uint8_t*>(bytes), static_cast<int32_t>(size), fingerprint.data());
    return fingerprint[0] ^ ((fingerprint[1] << 17) | (fingerprint[1] >> 47));
  }
  uint64_t hash = 1469598103934665603ULL;
  for (uint64_t index = 0; index < size; ++index) {
    hash ^= static_cast<uint8_t>(bytes[index]);
    hash *= 1099511628211ULL;
  }
  return hash;
}
uint64_t source_hash(EngineTrace& trace, VkBuffer buffer, const VkBufferImageCopy& region, uint64_t bytes) {
  if (!texture_upload_hash_trace_enabled() || bytes == 0 || bytes > 8ULL * 1024ULL * 1024ULL || region.bufferRowLength != 0 || region.bufferImageHeight != 0) return 0;
  const auto buffer_it = trace.buffers.find(reinterpret_cast<uintptr_t>(buffer));
  if (buffer_it == trace.buffers.end()) return 0;
  const auto memory_it = trace.mapped_memory.find(reinterpret_cast<uintptr_t>(buffer_it->second.memory));
  if (memory_it == trace.mapped_memory.end() || memory_it->second.size == VK_WHOLE_SIZE) return 0;
  const VkDeviceSize start = buffer_it->second.memory_offset + region.bufferOffset;
  if (start < memory_it->second.offset) return 0;
  const VkDeviceSize relative = start - memory_it->second.offset;
  if (relative > memory_it->second.size || bytes > memory_it->second.size - relative) return 0;
  const uint64_t started_ns = monotonic_ns();
  const uint64_t result = hash_upload_bytes(memory_it->second.base + relative, bytes);
  ++trace.uploads.fingerprint_calls;
  trace.uploads.fingerprint_ns += monotonic_ns() - started_ns;
  return result;
}
// One helper drives all hash-gated cold bookkeeping (same flag + lock, no hot-path cost).
template <typename Fn>
inline void tracked_hash_update(Fn&& fn) {
  if (!texture_upload_hash_trace_enabled()) return;
  with_engine_trace(std::forward<Fn>(fn));
}
void track_buffer_binding(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memory_offset) {
  tracked_hash_update([&](EngineTrace& t) { t.buffers[reinterpret_cast<uintptr_t>(buffer)] = TrackedBuffer{memory, memory_offset}; });
}
void forget_tracked_buffer(VkBuffer buffer) {
  tracked_hash_update([&](EngineTrace& t) { t.buffers.erase(reinterpret_cast<uintptr_t>(buffer)); });
}
void track_mapped_memory(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, void* data) {
  if (!data) return;
  tracked_hash_update([&](EngineTrace& t) { t.mapped_memory[reinterpret_cast<uintptr_t>(memory)] = MappedMemory{static_cast<const std::byte*>(data), offset, size}; });
}
void forget_mapped_memory(VkDeviceMemory memory) {
  tracked_hash_update([&](EngineTrace& t) { t.mapped_memory.erase(reinterpret_cast<uintptr_t>(memory)); });
}
void record_texture_upload(VkImage image, uint32_t region_count, const VkBufferImageCopy* regions, const std::vector<uint64_t>& content_hashes) {
  if (!texture_upload_trace_enabled() || !regions) return;
  EngineTrace& trace = engine_trace();
  std::scoped_lock lock(trace.mutex);
  const auto image_it = trace.images.find(reinterpret_cast<uintptr_t>(image));
  if (image_it == trace.images.end() || !(image_it->second.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return;
  for (uint32_t index = 0; index < region_count; ++index) {
    const VkBufferImageCopy& region = regions[index];
    const uint64_t bytes = estimate_region_bytes(image_it->second.format, region.imageExtent) * region.imageSubresource.layerCount;
    UploadSubresource key = make_upload_key(image_it->second.id, region);
    ++trace.uploads.regions;
    trace.uploads.bytes += bytes;
    const uint64_t content_hash = index < content_hashes.size() ? content_hashes[index] : 0;
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
  // Bound memory for long sessions; reset only starts a new duplicate-observation window.
  if (trace.seen_uploads.size() > 200000) trace.seen_uploads.clear();
}
void record_engine_event(EngineEvent event, uint64_t elapsed_ns, uint64_t units) {
  if (!engine_trace_enabled() && !intel_format_trace_enabled() && !texture_upload_trace_enabled()) return;
  EngineTrace& trace = engine_trace();
  std::array<EngineAggregate, static_cast<std::size_t>(EngineEvent::count)> report{};
  std::unordered_map<VkFormat, ImageFormatAggregate> format_report;
  UploadAggregate upload_report{};
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
  }
  for (std::size_t index = 0; index < report.size(); ++index) {
    const EngineAggregate& aggregate = report[index];
    if (!aggregate.calls) continue;
    std::fprintf(stderr, "nuah engine: vk=%s calls=%llu units=%llu total_us=%llu avg_us=%llu max_us=%llu\n", engine_event_name(static_cast<EngineEvent>(index)),
                 static_cast<unsigned long long>(aggregate.calls), static_cast<unsigned long long>(aggregate.units), static_cast<unsigned long long>(aggregate.total_ns / 1000ULL),
                 static_cast<unsigned long long>(aggregate.total_ns / aggregate.calls / 1000ULL), static_cast<unsigned long long>(aggregate.max_ns / 1000ULL));
  }
  for (const auto& [format, aggregate] : format_report) {
    if (!aggregate.images) continue;
    std::fprintf(stderr, "nuah intel-format: format=%s vk_format=%d images=%llu sampled=%llu mip_levels=%llu max_mips=%u texels=%llu estimated_mib=%llu\n", format_name(format), static_cast<int>(format),
                 static_cast<unsigned long long>(aggregate.images), static_cast<unsigned long long>(aggregate.sampled_images), static_cast<unsigned long long>(aggregate.mip_levels),
                 aggregate.max_mip_levels, static_cast<unsigned long long>(aggregate.texels), static_cast<unsigned long long>(aggregate.estimated_bytes / (1024ULL * 1024ULL)));
  }
  if (upload_report.regions) {
    std::fprintf(stderr, "nuah texture-upload: regions=%llu estimated_mib=%llu repeated_regions=%llu repeated_mib=%llu hashed_regions=%llu identical_regions=%llu identical_mib=%llu suppressed_regions=%llu suppressed_mib=%llu fingerprint_calls=%llu fingerprint_us=%llu\n",
                 static_cast<unsigned long long>(upload_report.regions), static_cast<unsigned long long>(upload_report.bytes / (1024ULL * 1024ULL)),
                 static_cast<unsigned long long>(upload_report.repeated_regions), static_cast<unsigned long long>(upload_report.repeated_bytes / (1024ULL * 1024ULL)),
                 static_cast<unsigned long long>(upload_report.hashed_regions), static_cast<unsigned long long>(upload_report.identical_regions),
                 static_cast<unsigned long long>(upload_report.identical_bytes / (1024ULL * 1024ULL)), static_cast<unsigned long long>(upload_report.suppressed_regions),
                 static_cast<unsigned long long>(upload_report.suppressed_bytes / (1024ULL * 1024ULL)), static_cast<unsigned long long>(upload_report.fingerprint_calls),
                 static_cast<unsigned long long>(upload_report.fingerprint_ns / 1000ULL));
  }
}
struct PresentMetrics {
  PresentMetrics() { interval_samples.reserve(240); }
  std::mutex mutex;
  uint64_t count = 0, interval_total_ns = 0, interval_max_ns = 0, call_total_ns = 0, call_max_ns = 0, previous_ns = 0, window_started_ns = 0, next_report_ns = 0;
  std::vector<uint64_t> interval_samples;
};
struct PresentReport {
  uint64_t count = 0, interval_count = 0, interval_total_ns = 0, interval_max_ns = 0, call_total_ns = 0, call_max_ns = 0, window_ns = 0, submit_calls = 0, submit_total_ns = 0;
  std::vector<uint64_t> interval_samples;
};
PresentMetrics& present_metrics() {
  static PresentMetrics value;
  return value;
}
// Submit attribution into the 1 Hz perf line (ANV execbuffer work invisible to present timing).
std::atomic<uint64_t> submit_perf_calls{0};
std::atomic<uint64_t> submit_perf_elapsed_ns{0};
void record_submit_perf(uint64_t elapsed_ns) {
  submit_perf_calls.fetch_add(1, std::memory_order_relaxed);
  submit_perf_elapsed_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}
uint64_t percentile(const std::vector<uint64_t>& sorted, uint64_t percent) {
  if (sorted.empty()) return 0;
  const std::size_t rank = static_cast<std::size_t>((sorted.size() * percent + 99ULL) / 100ULL);
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
    report.count = metrics.count;
    report.interval_count = metrics.interval_samples.size();
    report.interval_total_ns = metrics.interval_total_ns;
    report.interval_max_ns = metrics.interval_max_ns;
    report.call_total_ns = metrics.call_total_ns;
    report.call_max_ns = metrics.call_max_ns;
    report.window_ns = finished_ns - metrics.window_started_ns;
    report.submit_calls = submit_perf_calls.exchange(0, std::memory_order_relaxed);
    report.submit_total_ns = submit_perf_elapsed_ns.exchange(0, std::memory_order_relaxed);
    report.interval_samples.swap(metrics.interval_samples);
    metrics.count = metrics.interval_total_ns = metrics.interval_max_ns = metrics.call_total_ns = metrics.call_max_ns = 0;
    metrics.window_started_ns = finished_ns;
    metrics.next_report_ns = finished_ns + 1000000000ULL;
  }
  std::sort(report.interval_samples.begin(), report.interval_samples.end());
  const uint64_t p50_ns = percentile(report.interval_samples, 50);
  const uint64_t p95_ns = percentile(report.interval_samples, 95);
  const uint64_t p99_ns = percentile(report.interval_samples, 99);
  const auto count_over = [&](uint64_t threshold_ns) {
    return static_cast<uint64_t>(report.interval_samples.end() - std::upper_bound(report.interval_samples.begin(), report.interval_samples.end(), threshold_ns));
  };
  const uint64_t fps_milli = report.window_ns ? report.count * 1000000000000ULL / report.window_ns : 0;
  std::fprintf(stderr, "nuah perf: vulkan presents=%llu fps=%llu.%03llu avg_interval_us=%llu p50_us=%llu p95_us=%llu p99_us=%llu max_interval_us=%llu over33ms=%llu over50ms=%llu over100ms=%llu avg_call_us=%llu max_call_us=%llu submit_calls=%llu submit_avg_us=%llu\n",
               static_cast<unsigned long long>(report.count), static_cast<unsigned long long>(fps_milli / 1000ULL), static_cast<unsigned long long>(fps_milli % 1000ULL),
               static_cast<unsigned long long>(report.interval_count ? report.interval_total_ns / report.interval_count / 1000ULL : 0), static_cast<unsigned long long>(p50_ns / 1000ULL),
               static_cast<unsigned long long>(p95_ns / 1000ULL), static_cast<unsigned long long>(p99_ns / 1000ULL), static_cast<unsigned long long>(report.interval_max_ns / 1000ULL),
               static_cast<unsigned long long>(count_over(33333333ULL)), static_cast<unsigned long long>(count_over(50000000ULL)), static_cast<unsigned long long>(count_over(100000000ULL)),
               static_cast<unsigned long long>(report.call_total_ns / report.count / 1000ULL), static_cast<unsigned long long>(report.call_max_ns / 1000ULL),
               static_cast<unsigned long long>(report.submit_calls), static_cast<unsigned long long>(report.submit_calls ? report.submit_total_ns / report.submit_calls / 1000ULL : 0));
}
void trace_present_mode(const char* mode, bool available) {
  if (!bootstrap_trace_enabled()) return;
  std::fprintf(stderr, "nuah vulkan: requested present mode=%s %s\n", mode, available ? "enabled" : "unavailable; keeping host modes");
}
NuahNativeWindow* retain_window_for_surface(VkSurfaceKHR surface) {
  std::scoped_lock lock(surfaces_mutex);
  const auto found = surfaces.find(surface);
  if (found == surfaces.end() || !found->second.window) return nullptr;
  nuah_native_window_acquire(found->second.window);
  return found->second.window;
}
void normalize_surface_extent(VkSurfaceCapabilitiesKHR* capabilities, NuahNativeWindow* window) {
  if (!capabilities || !window) return;
  auto* host_window = static_cast<SDL_Window*>(nuah_native_window_host(window));
  int width = nuah_native_window_width(window);
  int height = nuah_native_window_height(window);
  if ((width <= 0 || height <= 0) && host_window) {
    if (SDL_GetWindowSize(host_window, &width, &height) <= 0 || width <= 0 || height <= 0) return;
  }
  const auto clamp_extent = [](uint32_t value, uint32_t minimum, uint32_t maximum) { return std::min(std::max(value, minimum), maximum); };
  // Wayland reports UINT32_MAX ("choose extent"); Android WSI reports realized size. Roblox expects the latter.
  if (surface_size_locked() || capabilities->currentExtent.width == UINT32_MAX || capabilities->currentExtent.height == UINT32_MAX || capabilities->currentExtent.width == 0 || capabilities->currentExtent.height == 0) {
    capabilities->currentExtent.width = clamp_extent(static_cast<uint32_t>(width), capabilities->minImageExtent.width, capabilities->maxImageExtent.width);
    capabilities->currentExtent.height = clamp_extent(static_cast<uint32_t>(height), capabilities->minImageExtent.height, capabilities->maxImageExtent.height);
  }
}
// Android WSI uses 4 swapchain images; keep host range authoritative, default to 4, allow A/B override.
void normalize_swapchain_depth(VkSurfaceCapabilitiesKHR* capabilities) {
  if (!capabilities) return;
  const char* raw = std::getenv("NUAH_VULKAN_MIN_IMAGE_COUNT");
  unsigned long wanted = 0;
  if (raw && *raw) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed < 1 || parsed > UINT32_MAX) return;
    wanted = parsed;
  } else {
    wanted = 4;
  }
  const uint32_t requested = static_cast<uint32_t>(wanted);
  if (requested <= capabilities->minImageCount || (capabilities->maxImageCount != 0 && requested > capabilities->maxImageCount)) return;
  capabilities->minImageCount = requested;
}
struct LazyLibrary {
  std::atomic<void*> handle{nullptr};
  std::atomic<unsigned> state{0};  // 0=unloaded, 1=loading, 2=finished
};
void* host_vulkan() {
  static LazyLibrary library;
  if (void* handle = library.handle.load(std::memory_order_acquire)) return handle;
  unsigned expected = 0;
  // Re-entrant dlopen would deadlock on a C++ guard; return bounded failure instead.
  if (!library.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return nullptr;
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
// Render-path callers must cache in a function-local `static`: per-call dlsym takes the loader lock (60-130 ms hitch).
bool host_has_instance_extension(const char* wanted) {
  if (!wanted) return false;
  const auto enumerate = host_function<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
  if (!enumerate) return false;
  uint32_t count = 0;
  if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS || count == 0) return false;
  std::vector<VkExtensionProperties> extensions(count);
  if (enumerate(nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
  for (const auto& extension : extensions)
    if (std::strcmp(extension.extensionName, wanted) == 0) return true;
  return false;
}
// Shared buffer-upload filter: keeps first occurrence, drops only proven repeats.
// Caller keeps zero-alloc fast path (early return before any vector use).
inline uint32_t filter_upload_regions(VkImage image, const VkBufferImageCopy* regions, uint32_t count, const std::vector<uint64_t>& hashes, std::vector<VkBufferImageCopy>& retained) {
  retained.clear();
  if (!texture_upload_dedup_enabled() || !regions || !count) return count;
  retained.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t hash = i < hashes.size() ? hashes[i] : 0;
    if (!should_suppress_upload(image, regions[i], hash)) retained.push_back(regions[i]);
  }
  return static_cast<uint32_t>(retained.size());
}
inline void finish_copy_to_image(VkImage image, uint32_t requested, const VkBufferImageCopy* regions, uint32_t forwarded, uint64_t forwarded_bytes, const std::vector<uint64_t>& hashes, uint64_t started_ns, bool do_copy_trace) {
  uint64_t texels = 0;
  for (uint32_t i = 0; i < requested; ++i) {
    const VkExtent3D e = regions ? regions[i].imageExtent : VkExtent3D{0, 0, 0};
    texels += static_cast<uint64_t>(e.width) * e.height * e.depth;
  }
  record_texture_upload(image, requested, regions, hashes);
  record_frame_copy(requested, forwarded, forwarded_bytes);
  record_engine_event(EngineEvent::copy_to_image, monotonic_ns() - started_ns, texels);
  if (do_copy_trace && copy_trace_log_slot()) std::fprintf(stderr, "nuah perf: vkCmdCopyBufferToImage regions=%u approx_texels=%llu (rate-limited)\n", requested, static_cast<unsigned long long>(texels));
}
inline void to_legacy_copy_regions(const VkBufferImageCopy2* src, uint32_t count, std::vector<VkBufferImageCopy>& dst) {
  dst.clear();
  if (!src || !count) return;
  dst.reserve(count);
  for (uint32_t i = 0; i < count; ++i) dst.push_back({src[i].bufferOffset, src[i].bufferRowLength, src[i].bufferImageHeight, src[i].imageSubresource, src[i].imageOffset, src[i].imageExtent});
}
}  // namespace
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info);
DEFINE_ENV_FLAG(pipeline_trace_enabled, "NUAH_VULKAN_PIPELINE_TRACE")
DEFINE_ENV_FLAG(pipeline_callsite_trace_enabled, "NUAH_VULKAN_CALLSITE_TRACE")
DEFINE_ENV_FLAG(wait_trace_enabled, "NUAH_VULKAN_WAIT_TRACE")
DEFINE_ENV_FLAG(submit_trace_enabled, "NUAH_VULKAN_SUBMIT_TRACE")
DEFINE_ENV_FLAG(copy_trace_enabled, "NUAH_VULKAN_COPY_TRACE")
bool residency_enabled() {
  static const bool value = [] {
    const char* raw = std::getenv("NUAH_VK_RESIDENCY");
    if (!raw || !*raw) return true;
    return std::strcmp(raw, "0") != 0 && std::strcmp(raw, "off") != 0 && std::strcmp(raw, "false") != 0 && std::strcmp(raw, "no") != 0;
  }();
  return value;
}
DEFINE_ENV_FLAG(memory_trace_enabled, "NUAH_VULKAN_MEMORY_TRACE")
void trace_memory_properties(VkPhysicalDevice physical_device, const VkPhysicalDeviceMemoryProperties& memory) {
  if (!memory_trace_enabled()) return;
  static std::mutex trace_mutex;
  static std::unordered_set<VkPhysicalDevice> traced;
  std::scoped_lock lock(trace_mutex);
  if (!traced.insert(physical_device).second) return;
  uint32_t host_visible_types = 0;
  for (uint32_t index = 0; index < memory.memoryTypeCount; ++index)
    if (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ++host_visible_types;
  std::fprintf(stderr, "nuah vulkan memory: heaps=%u types=%u host_visible_types=%u\n", memory.memoryHeapCount, memory.memoryTypeCount, host_visible_types);
  for (uint32_t index = 0; index < memory.memoryHeapCount; ++index) std::fprintf(stderr, "nuah vulkan memory: heap[%u] size=%llu flags=0x%x\n", index, static_cast<unsigned long long>(memory.memoryHeaps[index].size), memory.memoryHeaps[index].flags);
}
DEFINE_ENV_FLAG(residency_trace_enabled, "NUAH_VK_RESIDENCY_TRACE")
// Snapshotting a live caller cache serializes on the render thread; opt-in, persist at idle/teardown.
DEFINE_ENV_FLAG(residency_snapshot_enabled, "NUAH_VK_RESIDENCY_SNAPSHOT")
std::filesystem::path pipeline_cache_path() {
  const char* directory = std::getenv("NUAH_SHADER_CACHE_DIR");
  if (!directory || !*directory) directory = std::getenv("MESA_SHADER_CACHE_DIR");
  if (!directory || !*directory) {
    const char* home = std::getenv("HOME");
    if (home && *home) {
      static const auto default_dir = std::filesystem::path(home) / ".local" / "share" / "nuah" / "mesa-shader-cache";
      return default_dir / "nuah-vk-pipeline-cache.bin";
    }
    return {};
  }
  return std::filesystem::path(directory) / "nuah-vk-pipeline-cache.bin";
}
std::mutex pipeline_cache_file_mutex;
std::mutex persistent_pipeline_cache_mutex;
std::unordered_map<VkDevice, VkPipelineCache> persistent_pipeline_caches;
// Weak bookkeeping for caller-owned handles; serialize at idle/teardown without extending lifetime.
std::unordered_map<VkDevice, std::unordered_set<VkPipelineCache>> caller_pipeline_caches;
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
    output.write(static_cast<const char*>(raw_data), static_cast<std::streamsize>(size));
    if (!output) return;
  }
  std::filesystem::rename(temporary, path, error);
  if (error) std::filesystem::remove(temporary, error);
}
VkPipelineCache persistent_pipeline_cache(VkDevice device) {
  if (!device || !residency_enabled()) return VK_NULL_HANDLE;
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  const auto found = persistent_pipeline_caches.find(device);
  if (found != persistent_pipeline_caches.end()) return found->second;
  const auto function = host_function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache");
  if (!function) return VK_NULL_HANDLE;
  VkPipelineCacheCreateInfo create_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
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
  if (residency_trace_enabled()) std::fprintf(stderr, "nuah residency: created persistent pipeline cache seed_bytes=%zu path=%s\n", disk_data.size(), pipeline_cache_path().c_str());
  persistent_pipeline_caches.emplace(device, cache);
  return cache;
}
void save_pipeline_cache_handle(VkDevice device, VkPipelineCache cache) {
  if (!device || !cache || !residency_enabled()) return;
  const auto function = host_function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
  if (!function) return;
  std::size_t size = 0;
  if (function(device, cache, &size, nullptr) != VK_SUCCESS || size == 0 || size > 256 * 1024 * 1024) return;
  std::vector<uint8_t> data(size);
  if (function(device, cache, &size, data.data()) == VK_SUCCESS) save_pipeline_cache_data(data.data(), size);
}
static VkPipelineCache lookup_persistent_cache(VkDevice device) {
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  const auto found = persistent_pipeline_caches.find(device);
  return found != persistent_pipeline_caches.end() ? found->second : VK_NULL_HANDLE;
}
static std::vector<VkPipelineCache> snapshot_caller_caches(VkDevice device) {
  std::vector<VkPipelineCache> caches;
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  const auto found = caller_pipeline_caches.find(device);
  if (found != caller_pipeline_caches.end()) caches.assign(found->second.begin(), found->second.end());
  return caches;
}
static VkPipelineCache take_persistent_cache(VkDevice device) {
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  const auto found = persistent_pipeline_caches.find(device);
  if (found == persistent_pipeline_caches.end()) return VK_NULL_HANDLE;
  VkPipelineCache cache = found->second;
  persistent_pipeline_caches.erase(found);
  return cache;
}
void save_persistent_pipeline_cache(VkDevice device) {
  if (!device || !residency_enabled()) return;
  save_pipeline_cache_handle(device, lookup_persistent_cache(device));
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
  const std::vector<VkPipelineCache> caches = snapshot_caller_caches(device);
  for (VkPipelineCache cache : caches) save_pipeline_cache_handle(device, cache);
  if (residency_trace_enabled() && !caches.empty()) std::fprintf(stderr, "nuah residency: snapshotted caller pipeline caches=%zu\n", caches.size());
}
void clear_caller_pipeline_cache_tracking(VkDevice device) {
  if (!device) return;
  std::scoped_lock lock(persistent_pipeline_cache_mutex);
  caller_pipeline_caches.erase(device);
}
void destroy_persistent_pipeline_cache(VkDevice device, const VkAllocationCallbacks* allocator) {
  if (!device) return;
  save_persistent_pipeline_cache(device);
  VkPipelineCache cache = take_persistent_cache(device);
  if (!cache) return;
  const auto function = host_function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache");
  if (function) function(device, cache, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkPipelineCache* cache) {
  const auto function = host_function<PFN_vkCreatePipelineCache>("vkCreatePipelineCache");
  if (!function || !create_info || !cache) return VK_ERROR_INITIALIZATION_FAILED;
  VkPipelineCacheCreateInfo forwarded = *create_info;
  std::vector<uint8_t> disk_data;
  if (residency_enabled() && create_info->initialDataSize == 0 && create_info->pNext == nullptr) {
    disk_data = load_pipeline_cache_data();
    if (!disk_data.empty()) {
      forwarded.initialDataSize = disk_data.size();
      forwarded.pInitialData = disk_data.data();
    }
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(device, &forwarded, allocator, cache);
  if (result == VK_SUCCESS && *cache != VK_NULL_HANDLE) track_caller_pipeline_cache(device, *cache);
  if (pipeline_trace_enabled()) std::fprintf(stderr, "nuah perf: vkCreatePipelineCache seed_bytes=%zu elapsed_us=%llu result=%d\n", disk_data.size(), static_cast<unsigned long long>((monotonic_ns() - started_ns) / 1000ULL), static_cast<int>(result));
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache cache, std::size_t* data_size, void* data) {
  const auto function = host_function<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
  if (!function || !data_size) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = function(device, cache, data_size, data);
  if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && data && *data_size != 0) save_pipeline_cache_data(data, *data_size);
  return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache cache, const VkAllocationCallbacks* allocator) {
  save_pipeline_cache_handle(device, cache);
  untrack_caller_pipeline_cache(device, cache);
  const auto function = host_function<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache");
  if (function) function(device, cache, allocator);
}
}  // extern "C"
template <typename PFN, typename Info>
VkResult create_pipelines_impl(const char* fn_name, VkDevice device, VkPipelineCache cache, uint32_t count, const Info* create_info, const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
  const auto function = host_function<PFN>(fn_name);
  if (NUAH_UNLIKELY(!function)) return VK_ERROR_INITIALIZATION_FAILED;
  VkPipelineCache effective_cache = cache ? cache : persistent_pipeline_cache(device);
  if (NUAH_UNLIKELY(residency_trace_enabled())) std::fprintf(stderr, "nuah residency: %s cache_in=%p cache_used=%p count=%u\n", fn_name, reinterpret_cast<void*>(cache), reinterpret_cast<void*>(effective_cache), count);
  const bool trace = pipeline_trace_enabled() || pipeline_callsite_trace_enabled();
  const uint64_t started_ns = NUAH_UNLIKELY(trace) ? monotonic_ns() : 0;
  VkResult result = function(device, effective_cache, count, create_info, allocator, pipelines);
  if (NUAH_UNLIKELY(result != VK_SUCCESS && effective_cache != cache)) result = function(device, cache, count, create_info, allocator, pipelines);
  if (NUAH_UNLIKELY(result == VK_SUCCESS && residency_snapshot_enabled())) save_pipeline_cache_handle(device, effective_cache);
  if (NUAH_UNLIKELY(trace)) {
    const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
    if (pipeline_trace_enabled() && elapsed_us >= 1000ULL) std::fprintf(stderr, "nuah perf: %s count=%u elapsed_us=%llu result=%d\n", fn_name, count, static_cast<unsigned long long>(elapsed_us), static_cast<int>(result));
    if (pipeline_callsite_trace_enabled() && elapsed_us >= 1000ULL) {
      const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
      std::fprintf(stderr, "nuah perf: %s caller=0x%llx count=%u elapsed_us=%llu result=%d\n", fn_name, static_cast<unsigned long long>(caller), count, static_cast<unsigned long long>(elapsed_us), static_cast<int>(result));
    }
  }
  return result;
}
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
  return create_pipelines_impl<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines", device, cache, count, create_info, allocator, pipelines); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
  return create_pipelines_impl<PFN_vkCreateComputePipelines>("vkCreateComputePipelines", device, cache, count, create_info, allocator, pipelines); }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
  NUAH_VK_HOST(PFN_vkQueueWaitIdle, vkQueueWaitIdle, VK_ERROR_INITIALIZATION_FAILED);
  if (!wait_trace_enabled()) return nuah_host_fn(queue);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = nuah_host_fn(queue);
  const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
  if (elapsed_us >= 1000ULL) std::fprintf(stderr, "nuah perf: vkQueueWaitIdle elapsed_us=%llu result=%d\n", static_cast<unsigned long long>(elapsed_us), static_cast<int>(result));
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
  NUAH_VK_HOST(PFN_vkDeviceWaitIdle, vkDeviceWaitIdle, VK_ERROR_INITIALIZATION_FAILED);
  const bool trace = wait_trace_enabled();
  const uint64_t started_ns = trace ? monotonic_ns() : 0;
  const VkResult result = nuah_host_fn(device);
  save_persistent_pipeline_cache(device);
  save_caller_pipeline_caches(device);
  if (trace) {
    const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
    if (elapsed_us >= 1000ULL) std::fprintf(stderr, "nuah perf: vkDeviceWaitIdle elapsed_us=%llu result=%d\n", static_cast<unsigned long long>(elapsed_us), static_cast<int>(result));
  }
  return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
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
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t count, const VkFence* fences, VkBool32 wait_all, uint64_t timeout) {
  NUAH_VK_HOST(PFN_vkWaitForFences, vkWaitForFences, VK_ERROR_INITIALIZATION_FAILED);
  if (!wait_trace_enabled() && !engine_trace_enabled()) return nuah_host_fn(device, count, fences, wait_all, timeout);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = nuah_host_fn(device, count, fences, wait_all, timeout);
  const uint64_t elapsed_ns = monotonic_ns() - started_ns;
  record_engine_event(EngineEvent::wait_fence, elapsed_ns, count);
  if (wait_trace_enabled() && elapsed_ns / 1000ULL >= 1000ULL)
    std::fprintf(stderr, "nuah perf: vkWaitForFences count=%u timeout=%llu elapsed_us=%llu result=%d\n", count, static_cast<unsigned long long>(timeout), static_cast<unsigned long long>(elapsed_ns / 1000ULL), static_cast<int>(result));
  return result;
}
}  // extern "C"
template <typename PFN, typename Info, typename CountFn>
inline VkResult queue_submit_impl(PFN function, const char* name, VkQueue queue, uint32_t submit_count, const Info* submits, VkFence fence, CountFn get_cb_count) {
  if (!function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!submit_trace_enabled() && !engine_trace_enabled() && !frame_work_trace_enabled()) {
    const uint64_t started_ns = monotonic_ns();
    const VkResult result = function(queue, submit_count, submits, fence);
    record_submit_perf(monotonic_ns() - started_ns);
    return result;
  }
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = function(queue, submit_count, submits, fence);
  uint64_t command_buffers = 0;
  for (uint32_t index = 0; index < submit_count; ++index) command_buffers += submits ? get_cb_count(submits[index]) : 0;
  if (result == VK_SUCCESS) record_frame_submit(submit_count, command_buffers);
  record_engine_event(EngineEvent::submit, monotonic_ns() - started_ns, command_buffers);
  if (submit_trace_enabled()) {
    const uint64_t elapsed_us = (monotonic_ns() - started_ns) / 1000ULL;
    std::fprintf(stderr, "nuah perf: %s batches=%u command_buffers=%llu elapsed_us=%llu result=%d\n", name, submit_count, static_cast<unsigned long long>(command_buffers),
                 static_cast<unsigned long long>(elapsed_us), static_cast<int>(result));
  }
  return result;
}
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submit_count, const VkSubmitInfo* submits, VkFence fence) {
  static const auto function = host_function<PFN_vkQueueSubmit>("vkQueueSubmit");
  return queue_submit_impl(function, "vkQueueSubmit", queue, submit_count, submits, fence, [](const VkSubmitInfo& s) { return s.commandBufferCount; });
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t submit_count, const VkSubmitInfo2* submits, VkFence fence) {
  static const auto function = host_function<PFN_vkQueueSubmit2>("vkQueueSubmit2");
  return queue_submit_impl(function, "vkQueueSubmit2", queue, submit_count, submits, fence, [](const VkSubmitInfo2& s) { return s.commandBufferInfoCount; });
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkImage* image) {
  NUAH_VK_HOST(PFN_vkCreateImage, vkCreateImage, VK_ERROR_INITIALIZATION_FAILED);
  if (!intel_format_trace_enabled() && !texture_upload_dedup_enabled() && !engine_trace_enabled()) return nuah_host_fn(device, create_info, allocator, image);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = nuah_host_fn(device, create_info, allocator, image);
  if (result == VK_SUCCESS) {
    record_image_format(create_info);
    record_tracked_image(image ? *image : VK_NULL_HANDLE, create_info);
  }
  record_engine_event(EngineEvent::image_create, monotonic_ns() - started_ns);
  return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
  NUAH_VK_FORGET_VOID(PFN_vkDestroyImage, vkDestroyImage, forget_tracked_image(image), device, image, allocator); }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memory_offset) {
  NUAH_VK_TRACKED_RESULT(PFN_vkBindBufferMemory, vkBindBufferMemory, VK_ERROR_INITIALIZATION_FAILED, track_buffer_binding(buffer, memory, memory_offset), device, buffer, memory, memory_offset); }
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator) {
  NUAH_VK_FORGET_VOID(PFN_vkDestroyBuffer, vkDestroyBuffer, forget_tracked_buffer(buffer), device, buffer, allocator); }
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** data) {
  NUAH_VK_TRACKED_RESULT(PFN_vkMapMemory, vkMapMemory, VK_ERROR_MEMORY_MAP_FAILED, track_mapped_memory(memory, offset, size, data ? *data : nullptr), device, memory, offset, size, flags, data); }
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
  NUAH_VK_FORGET_VOID(PFN_vkUnmapMemory, vkUnmapMemory, forget_mapped_memory(memory), device, memory); }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer command_buffer, VkBuffer source, VkImage destination, VkImageLayout layout, uint32_t region_count, const VkBufferImageCopy* regions) {
  NUAH_VK_HOST(PFN_vkCmdCopyBufferToImage, vkCmdCopyBufferToImage);
  if (!texture_upload_dedup_enabled() && !texture_upload_trace_enabled() && !copy_trace_enabled() && !engine_trace_enabled() && !frame_work_trace_enabled()) {
    nuah_host_fn(command_buffer, source, destination, layout, region_count, regions);
    return;
  }
  const uint64_t started_ns = monotonic_ns();
  const bool do_copy_trace = copy_trace_enabled();
  const std::vector<uint64_t> content_hashes = collect_upload_hashes(source, destination, region_count, regions);
  std::vector<VkBufferImageCopy> retained_regions;
  const uint32_t forwarded_count = filter_upload_regions(destination, regions, region_count, content_hashes, retained_regions);
  const VkBufferImageCopy* forwarded_regions = (forwarded_count != region_count) ? (retained_regions.empty() ? nullptr : retained_regions.data()) : regions;
  // When dedup is off, filter returns count unchanged and forwarded aliases input (no alloc used).
  const uint64_t forwarded_bytes = estimate_frame_copy_bytes(destination, forwarded_count, forwarded_regions);
  if (forwarded_count) nuah_host_fn(command_buffer, source, destination, layout, forwarded_count, forwarded_regions);
  finish_copy_to_image(destination, region_count, regions, forwarded_count, forwarded_bytes, content_hashes, started_ns, do_copy_trace);
}
// Vulkan 1.3 copy: same conservative dedup as legacy (sampled+mapped+proven repeat; first always forwards).
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(VkCommandBuffer command_buffer, const VkCopyBufferToImageInfo2* copy_info) {
  NUAH_VK_HOST(PFN_vkCmdCopyBufferToImage2, vkCmdCopyBufferToImage2);
  if (!copy_info) return;
  if (!texture_upload_dedup_enabled() && !texture_upload_trace_enabled() && !copy_trace_enabled() && !engine_trace_enabled() && !frame_work_trace_enabled()) {
    nuah_host_fn(command_buffer, copy_info);
    return;
  }
  const uint64_t started_ns = monotonic_ns();
  std::vector<VkBufferImageCopy> legacy_regions;
  to_legacy_copy_regions(copy_info->pRegions, copy_info->regionCount, legacy_regions);
  const std::vector<uint64_t> content_hashes = collect_upload_hashes(copy_info->srcBuffer, copy_info->dstImage, copy_info->regionCount, legacy_regions.empty() ? nullptr : legacy_regions.data());
  std::vector<VkBufferImageCopy2> retained_regions;
  std::vector<VkBufferImageCopy> retained_legacy_regions;
  const VkBufferImageCopy2* forwarded_regions = copy_info->pRegions;
  uint32_t forwarded_count = copy_info->regionCount;
  if (texture_upload_dedup_enabled() && copy_info->pRegions && copy_info->regionCount) {
    retained_regions.reserve(copy_info->regionCount);
    retained_legacy_regions.reserve(copy_info->regionCount);
    for (uint32_t index = 0; index < copy_info->regionCount; ++index) {
      const uint64_t content_hash = index < content_hashes.size() ? content_hashes[index] : 0;
      if (!should_suppress_upload(copy_info->dstImage, legacy_regions[index], content_hash)) {
        retained_regions.push_back(copy_info->pRegions[index]);
        retained_legacy_regions.push_back(legacy_regions[index]);
      }
    }
    forwarded_count = static_cast<uint32_t>(retained_regions.size());
    forwarded_regions = retained_regions.empty() ? nullptr : retained_regions.data();
  }
  VkCopyBufferToImageInfo2 forwarded_info = *copy_info;
  forwarded_info.regionCount = forwarded_count;
  forwarded_info.pRegions = forwarded_regions;
  const VkBufferImageCopy* byte_regions = !retained_legacy_regions.empty() ? retained_legacy_regions.data() : (legacy_regions.empty() ? nullptr : legacy_regions.data());
  const uint64_t forwarded_bytes = estimate_frame_copy_bytes(copy_info->dstImage, forwarded_count, byte_regions);
  if (forwarded_count) nuah_host_fn(command_buffer, &forwarded_info);
  record_texture_upload(copy_info->dstImage, copy_info->regionCount, legacy_regions.empty() ? nullptr : legacy_regions.data(), content_hashes);
  record_frame_copy(copy_info->regionCount, forwarded_count, forwarded_bytes);
  record_engine_event(EngineEvent::copy_to_image, monotonic_ns() - started_ns, copy_info->regionCount);
  if (copy_trace_enabled() && copy_trace_log_slot()) { std::fprintf(stderr, "nuah perf: vkCmdCopyBufferToImage2 regions=%u forwarded=%u (rate-limited)\n", copy_info->regionCount, forwarded_count); }
}
// Observational image-copy accounting shared by vkCmdCopyImage paths (no dedup, layout-preserving).
}  // extern "C"
template <typename R>
void trace_image_copy_perf(const char* name, uint64_t started_ns, uint32_t count, const R* regions) {
  uint64_t texels = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const VkExtent3D ext = regions ? regions[i].extent : VkExtent3D{0, 0, 0};
    texels += static_cast<uint64_t>(ext.width) * ext.height * ext.depth;
  }
  record_engine_event(EngineEvent::copy_to_image, monotonic_ns() - started_ns, count);
  if (copy_trace_enabled() && copy_trace_log_slot()) std::fprintf(stderr, "nuah perf: %s regions=%u approx_texels=%llu\n", name, count, static_cast<unsigned long long>(texels));
}
extern "C" {
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer command_buffer, const VkCopyImageInfo2* copy_info) {
  NUAH_VK_HOST(PFN_vkCmdCopyImage2, vkCmdCopyImage2);
  if (!copy_info) return;
  if (NUAH_LIKELY(!copy_trace_enabled() && !engine_trace_enabled())) {
    nuah_host_fn(command_buffer, copy_info);
    return;
  }
  const uint64_t nuah_started_ns = monotonic_ns();
  nuah_host_fn(command_buffer, copy_info);
  trace_image_copy_perf("vkCmdCopyImage2", nuah_started_ns, copy_info->regionCount, copy_info->pRegions);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer command_buffer, VkImage source, VkImageLayout source_layout, VkImage destination, VkImageLayout destination_layout, uint32_t region_count, const VkImageCopy* regions) {
  NUAH_VK_COPY_IMAGE(PFN_vkCmdCopyImage, vkCmdCopyImage, "vkCmdCopyImage", region_count, regions, command_buffer, source, source_layout, destination, destination_layout, region_count,
                     regions);
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* allocate_info, VkDescriptorSet* descriptor_sets) {
  NUAH_VK_HOST(PFN_vkAllocateDescriptorSets, vkAllocateDescriptorSets, VK_ERROR_INITIALIZATION_FAILED);
  const uint32_t batch_size = descriptor_alloc_batch_size();
  const bool trace = descriptor_alloc_trace_enabled() || engine_trace_enabled();
  if (!batch_size && !trace) return nuah_host_fn(device, allocate_info, descriptor_sets);
  ++descriptor_alloc_calls;
  const uint64_t started_ns = monotonic_ns();
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  const bool batchable = batch_size && allocate_info && descriptor_sets && allocate_info->descriptorSetCount && allocate_info->pSetLayouts && !allocate_info->pNext;
  if (!batchable) {
    ++descriptor_alloc_driver_calls;
    descriptor_alloc_driver_sets += allocate_info ? allocate_info->descriptorSetCount : 0;
    result = nuah_host_fn(device, allocate_info, descriptor_sets);
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
      if (take_cached_descriptor_set(allocate_info->descriptorPool, layout, &cached)) {
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
      const uint32_t extra_count = std::min<uint32_t>(batch_size, static_cast<uint32_t>(missing_layouts.size()));
      for (uint32_t index = 0; index < extra_count; ++index) forwarded_layouts.push_back(missing_layouts[index]);
      std::vector<VkDescriptorSet> forwarded_sets(forwarded_layouts.size(), VK_NULL_HANDLE);
      VkDescriptorSetAllocateInfo forwarded_info = *allocate_info;
      forwarded_info.descriptorSetCount = static_cast<uint32_t>(forwarded_layouts.size());
      forwarded_info.pSetLayouts = forwarded_layouts.data();
      ++descriptor_alloc_driver_calls;
      descriptor_alloc_driver_sets += forwarded_info.descriptorSetCount;
      result = nuah_host_fn(device, &forwarded_info, forwarded_sets.data());
      if (result != VK_SUCCESS && extra_count) {
        // Pool may fit the request but not the speculative batch; retry exact (Vulkan guarantees atomic alloc).
        forwarded_info.descriptorSetCount = static_cast<uint32_t>(missing_layouts.size());
        forwarded_info.pSetLayouts = missing_layouts.data();
        forwarded_sets.resize(missing_layouts.size());
        ++descriptor_alloc_driver_calls;
        descriptor_alloc_driver_sets += forwarded_info.descriptorSetCount;
        result = nuah_host_fn(device, &forwarded_info, forwarded_sets.data());
      }
      if (result == VK_SUCCESS) {
        for (std::size_t index = 0; index < missing_indices.size(); ++index) descriptor_sets[missing_indices[index]] = forwarded_sets[index];
        const std::size_t allocated_count = missing_layouts.size();
        if (forwarded_sets.size() > allocated_count) {
          std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>> extras;
          extras.reserve(forwarded_sets.size() - allocated_count);
          for (std::size_t index = allocated_count; index < forwarded_sets.size(); ++index) extras.emplace_back(forwarded_layouts[index], forwarded_sets[index]);
          const std::vector<VkDescriptorSet> discard = retain_batched_descriptor_sets(allocate_info->descriptorPool, extras);
          if (!discard.empty()) {
            static const auto free_function = host_function<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets");
            if (free_function) free_function(device, allocate_info->descriptorPool, static_cast<uint32_t>(discard.size()), discard.data());
          }
          descriptor_alloc_batched_sets += extras.size() - discard.size();
        }
      } else {
        restore_cached_descriptor_sets(allocate_info->descriptorPool, consumed);
      }
    }
  }
  record_engine_event(EngineEvent::descriptor_allocate, monotonic_ns() - started_ns, allocate_info ? allocate_info->descriptorSetCount : 0);
  maybe_report_descriptor_alloc();
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptor_pool, uint32_t descriptor_set_count, const VkDescriptorSet* descriptor_sets) {
  NUAH_VK_TRACED_RESULT(PFN_vkFreeDescriptorSets, vkFreeDescriptorSets, EngineEvent::descriptor_free, descriptor_set_count, device, descriptor_pool, descriptor_set_count, descriptor_sets); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkDescriptorPool* descriptor_pool) {
  NUAH_VK_TRACED_RESULT(PFN_vkCreateDescriptorPool, vkCreateDescriptorPool, EngineEvent::descriptor_pool_create, create_info ? create_info->maxSets : 0, device, create_info, allocator, descriptor_pool); }
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptor_pool, VkDescriptorPoolResetFlags flags) {
  NUAH_VK_HOST(PFN_vkResetDescriptorPool, vkResetDescriptorPool, VK_ERROR_INITIALIZATION_FAILED);
  const bool trace = engine_trace_enabled();
  const uint64_t started_ns = trace ? monotonic_ns() : 0;
  const VkResult result = nuah_host_fn(device, descriptor_pool, flags);
  if (descriptor_alloc_batch_size() && result == VK_SUCCESS) clear_cached_descriptor_pool(descriptor_pool);
  if (trace) record_engine_event(EngineEvent::descriptor_pool_reset, monotonic_ns() - started_ns);
  return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptor_pool, const VkAllocationCallbacks* allocator) {
  NUAH_VK_CMD_VOID(PFN_vkDestroyDescriptorPool, vkDestroyDescriptorPool, if (descriptor_alloc_batch_size()) clear_cached_descriptor_pool(descriptor_pool), device, descriptor_pool, allocator); }
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t descriptor_write_count, const VkWriteDescriptorSet* descriptor_writes, uint32_t descriptor_copy_count, const VkCopyDescriptorSet* descriptor_copies) {
  NUAH_VK_TRACED_VOID(PFN_vkUpdateDescriptorSets, vkUpdateDescriptorSets, EngineEvent::descriptor_update, static_cast<uint64_t>(descriptor_write_count) + descriptor_copy_count, device,
                      descriptor_write_count, descriptor_writes, descriptor_copy_count, descriptor_copies); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkImageView* image_view) {
  NUAH_VK_TRACED_RESULT(PFN_vkCreateImageView, vkCreateImageView, EngineEvent::image_view_create, 1, device, create_info, allocator, image_view); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkSampler* sampler) {
  NUAH_VK_HOST(PFN_vkCreateSampler, vkCreateSampler, VK_ERROR_INITIALIZATION_FAILED);
  const float min_lod = texture_min_lod();
  if (!create_info || min_lod == 0.0f || create_info->unnormalizedCoordinates == VK_TRUE || create_info->maxLod <= create_info->minLod + min_lod) return nuah_host_fn(device, create_info, allocator, sampler);
  VkSamplerCreateInfo adjusted = *create_info;
  adjusted.minLod = std::min(create_info->maxLod, create_info->minLod + min_lod);
  adjusted.mipLodBias += min_lod;
  static std::atomic<uint64_t> adjusted_samplers{0};
  const uint64_t number = ++adjusted_samplers;
  if (texture_min_lod_trace_enabled() && number <= 8)
    std::fprintf(stderr, "nuah texture-lod: sampler=%llu old_min_lod=%.2f new_min_lod=%.2f old_bias=%.2f new_bias=%.2f\n", static_cast<unsigned long long>(number), create_info->minLod, adjusted.minLod, create_info->mipLodBias, adjusted.mipLodBias);
  return nuah_host_fn(device, &adjusted, allocator, sampler);
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer command_buffer, const VkCommandBufferBeginInfo* begin_info) {
  NUAH_VK_CMD_RESULT(PFN_vkBeginCommandBuffer, vkBeginCommandBuffer, clear_cmd_caches(command_buffer), command_buffer, begin_info); }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer command_buffer) {
  NUAH_VK_CMD_RESULT(PFN_vkEndCommandBuffer, vkEndCommandBuffer, clear_cmd_caches(command_buffer), command_buffer); }
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer command_buffer, VkCommandBufferResetFlags flags) {
  NUAH_VK_CMD_RESULT(PFN_vkResetCommandBuffer, vkResetCommandBuffer, clear_cmd_caches_on_success(command_buffer, nuah_result), command_buffer, flags); }
// Bulk erase with one lock per cache; per-cb clear would take N locks.
void erase_many_cmd_caches(uint32_t count, const VkCommandBuffer* buffers) {
  if (!buffers) return;
  if (descriptor_bind_dedup_enabled()) {
    DescriptorBindState& state = descriptor_bind_state();
    std::scoped_lock lock(state.mutex);
    for (uint32_t i = 0; i < count; ++i) state.last_bind.erase(reinterpret_cast<uintptr_t>(buffers[i]));
  }
  if (command_state_dedup_enabled()) {
    CommandStateCache& cache = command_state_cache();
    std::scoped_lock lock(cache.mutex);
    for (uint32_t i = 0; i < count; ++i) cache.last_state.erase(reinterpret_cast<uintptr_t>(buffers[i]));
  }
}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool command_pool, uint32_t command_buffer_count, const VkCommandBuffer* command_buffers) {
  NUAH_VK_CMD_VOID(PFN_vkFreeCommandBuffers, vkFreeCommandBuffers, erase_many_cmd_caches(command_buffer_count, command_buffers), device, command_pool, command_buffer_count, command_buffers); }
VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer command_buffer, uint32_t command_buffer_count, const VkCommandBuffer* command_buffers) {
  NUAH_VK_CMD_VOID(PFN_vkCmdExecuteCommands, vkCmdExecuteCommands, clear_cmd_caches(command_buffer), command_buffer, command_buffer_count, command_buffers); }
}  // extern "C"
template <typename Fn, typename... Args>
void apply_command_state(VkCommandBuffer cb, CommandStateSnapshot&& snapshot, Fn&& fn, Args&&... args) {
  ++command_state_calls;
  if (command_state_is_duplicate(cb, snapshot)) {
    ++command_state_suppressed;
    maybe_report_command_state();
    return;
  }
  fn(std::forward<Args>(args)...);
  remember_command_state(cb, std::move(snapshot));
  maybe_report_command_state();
}
extern "C" {
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point, VkPipeline pipeline) {
  NUAH_VK_BIND_STATE(PFN_vkCmdBindPipeline, vkCmdBindPipeline, command_buffer, make_pipeline_snapshot(bind_point, pipeline), bind_point, pipeline); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer command_buffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType index_type) {
  NUAH_VK_BIND_STATE(PFN_vkCmdBindIndexBuffer, vkCmdBindIndexBuffer, command_buffer, make_index_snapshot(buffer, offset, index_type), buffer, offset, index_type); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer command_buffer, uint32_t first_binding, uint32_t binding_count, const VkBuffer* buffers, const VkDeviceSize* offsets) {
  NUAH_VK_BIND_STATE(PFN_vkCmdBindVertexBuffers, vkCmdBindVertexBuffers, command_buffer, make_vertex_snapshot(first_binding, buffers, offsets, binding_count), first_binding, binding_count, buffers, offsets); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer command_buffer, uint32_t first_binding, uint32_t binding_count, const VkBuffer* buffers, const VkDeviceSize* offsets, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
  NUAH_VK_BIND_STATE(PFN_vkCmdBindVertexBuffers2, vkCmdBindVertexBuffers2, command_buffer, make_vertex2_snapshot(first_binding, buffers, offsets, sizes, strides, binding_count), first_binding, binding_count, buffers, offsets, sizes, strides); }
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer command_buffer, VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set, uint32_t descriptor_set_count, const VkDescriptorSet* descriptor_sets, uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets) {
  NUAH_VK_HOST(PFN_vkCmdBindDescriptorSets, vkCmdBindDescriptorSets);
  const bool dedup = descriptor_bind_dedup_enabled();
  const bool trace = engine_trace_enabled();
  if (NUAH_LIKELY(!dedup && !trace)) {
    nuah_host_fn(command_buffer, bind_point, layout, first_set, descriptor_set_count, descriptor_sets, dynamic_offset_count, dynamic_offsets);
    return;
  }
  if (dedup) {
    ++descriptor_bind_calls;
    DescriptorBindState& state = descriptor_bind_state();
    bool duplicate = false;
    {
      std::scoped_lock lock(state.mutex);
      const auto found = state.last_bind.find(reinterpret_cast<uintptr_t>(command_buffer));
      if (found != state.last_bind.end()) duplicate = descriptor_bind_matches(found->second, bind_point, layout, first_set, descriptor_set_count, descriptor_sets, dynamic_offset_count, dynamic_offsets);
    }
    if (duplicate) {
      ++descriptor_bind_suppressed;
      maybe_report_descriptor_bind_dedup();
      return;
    }
  }
  const uint64_t started_ns = trace ? monotonic_ns() : 0;
  nuah_host_fn(command_buffer, bind_point, layout, first_set, descriptor_set_count, descriptor_sets, dynamic_offset_count, dynamic_offsets);
  if (dedup) remember_descriptor_bind(command_buffer, bind_point, layout, first_set, descriptor_set_count, descriptor_sets, dynamic_offset_count, dynamic_offsets);
  if (dedup) maybe_report_descriptor_bind_dedup();
  if (trace) record_engine_event(EngineEvent::descriptor_bind, monotonic_ns() - started_ns, descriptor_set_count);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer command_buffer, VkPipelineStageFlags source_stage_mask, VkPipelineStageFlags destination_stage_mask, VkDependencyFlags dependency_flags, uint32_t memory_barrier_count, const VkMemoryBarrier* memory_barriers, uint32_t buffer_memory_barrier_count, const VkBufferMemoryBarrier* buffer_memory_barriers, uint32_t image_memory_barrier_count, const VkImageMemoryBarrier* image_memory_barriers) {
  NUAH_VK_TRACED_VOID(PFN_vkCmdPipelineBarrier, vkCmdPipelineBarrier, EngineEvent::barrier, static_cast<uint64_t>(memory_barrier_count) + buffer_memory_barrier_count + image_memory_barrier_count, command_buffer,
                      source_stage_mask, destination_stage_mask, dependency_flags, memory_barrier_count, memory_barriers, buffer_memory_barrier_count, buffer_memory_barriers, image_memory_barrier_count,
                      image_memory_barriers); }
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* image_index) {
  NUAH_VK_HOST(PFN_vkAcquireNextImageKHR, vkAcquireNextImageKHR, VK_ERROR_INITIALIZATION_FAILED);
  if (NUAH_LIKELY(!wait_trace_enabled() && !engine_trace_enabled())) return nuah_host_fn(device, swapchain, timeout, semaphore, fence, image_index);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = nuah_host_fn(device, swapchain, timeout, semaphore, fence, image_index);
  const uint64_t elapsed_ns = monotonic_ns() - started_ns;
  record_engine_event(EngineEvent::acquire, elapsed_ns);
  if (wait_trace_enabled() && elapsed_ns / 1000ULL >= 1000ULL)
    std::fprintf(stderr, "nuah perf: vkAcquireNextImageKHR timeout=%llu elapsed_us=%llu result=%d\n", static_cast<unsigned long long>(timeout), static_cast<unsigned long long>(elapsed_ns / 1000ULL), static_cast<int>(result));
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(VkInstance instance, const AndroidSurfaceCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkSurfaceKHR* output) {
  nuah_android_api_register("libvulkan.so", "vkCreateAndroidSurfaceKHR", NUAH_ANDROID_API_TRANSLATED);
  if (!instance || !create_info || !output || create_info->sType != VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR || !create_info->window) { return VK_ERROR_INITIALIZATION_FAILED; }
  auto* host_window = static_cast<SDL_Window*>(nuah_native_window_host(create_info->window));
  if (!host_window) return VK_ERROR_INITIALIZATION_FAILED;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  // Prefer SDL Wayland objects at the exact Wayland boundary; SDL_CreateSurface stays as X11 fallback.
  const auto properties = SDL_GetWindowProperties(host_window);
  auto* display = static_cast<wl_display*>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
  auto* wayland_surface = static_cast<wl_surface*>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
  const auto create_wayland = host_function<PFN_vkCreateWaylandSurfaceKHR>("vkCreateWaylandSurfaceKHR");
  if (display && wayland_surface && create_wayland) {
    VkWaylandSurfaceCreateInfoKHR wayland_info{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, nullptr, 0, display, wayland_surface};
    if (create_wayland(instance, &wayland_info, allocator, &surface) != VK_SUCCESS) { return VK_ERROR_INITIALIZATION_FAILED; }
  } else if (!SDL_Vulkan_CreateSurface(host_window, instance, allocator, &surface)) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  bool inserted = false;
  {
    // Lock order: surface table outermost, then window registry (matches retain path; avoids deadlock).
    std::scoped_lock lock(surfaces_mutex);
    nuah_native_window_acquire(create_info->window);
    inserted = surfaces.emplace(surface, SurfaceOwner{.instance = instance, .window = create_info->window}).second;
  }
  if (!inserted) {
    SDL_Vulkan_DestroySurface(instance, surface, allocator);
    nuah_native_window_release(create_info->window);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  *output = surface;
  if (bootstrap_trace_enabled()) { std::fprintf(stderr, "nuah vulkan: Android surface translated to host surface\n"); }
  return VK_SUCCESS;
}
// bionic_ aliases force Android lookups through Nuah's Wayland/SDL adapter (not ATL's GTK path).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name);
VKAPI_ATTR VkResult VKAPI_CALL bionic_vkCreateAndroidSurfaceKHR(VkInstance instance, const AndroidSurfaceCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkSurfaceKHR* output) {
  return vkCreateAndroidSurfaceKHR(instance, create_info, allocator, output);
}
static NuahNativeWindow* take_surface_locked(VkSurfaceKHR surface, VkInstance* instance_inout) {
  std::scoped_lock lock(surfaces_mutex);
  const auto found = surfaces.find(surface);
  if (found == surfaces.end()) return nullptr;
  NuahNativeWindow* window = found->second.window;
  if (instance_inout) *instance_inout = found->second.instance;
  surfaces.erase(found);
  return window;
}
static std::vector<std::pair<VkSurfaceKHR, NuahNativeWindow*>> take_instance_surfaces(VkInstance instance) {
  std::vector<std::pair<VkSurfaceKHR, NuahNativeWindow*>> owned;
  std::scoped_lock lock(surfaces_mutex);
  for (auto it = surfaces.begin(); it != surfaces.end();) {
    if (it->second.instance == instance) {
      owned.emplace_back(it->first, it->second.window);
      it = surfaces.erase(it);
    } else {
      ++it;
    }
  }
  return owned;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator) {
  NuahNativeWindow* window = take_surface_locked(surface, &instance);
  SDL_Vulkan_DestroySurface(instance, surface, allocator);
  nuah_native_window_release(window);
}
static void normalize_caps_and_release(VkSurfaceCapabilitiesKHR* caps, VkSurfaceKHR surface);
static inline VkResult normalize_result(VkResult result, VkSurfaceCapabilitiesKHR* caps, VkSurfaceKHR surface);
static void normalize_caps_and_release(VkSurfaceCapabilitiesKHR* caps, VkSurfaceKHR surface) {
  NuahNativeWindow* window = retain_window_for_surface(surface);
  normalize_surface_extent(caps, window);
  normalize_swapchain_depth(caps);
  if (window) nuah_native_window_release(window);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical_device, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* capabilities) {
  NUAH_VK_SURFACE_RESULT(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, vkGetPhysicalDeviceSurfaceCapabilitiesKHR, capabilities, surface, false, physical_device, surface, capabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physical_device, const VkPhysicalDeviceSurfaceInfo2KHR* surface_info, VkSurfaceCapabilities2KHR* capabilities) {
  NUAH_VK_SURFACE_RESULT(PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR, vkGetPhysicalDeviceSurfaceCapabilities2KHR, (capabilities ? &capabilities->surfaceCapabilities : nullptr),
                         surface_info->surface, (!surface_info), physical_device, surface_info, capabilities);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical_device, VkSurfaceKHR surface, uint32_t* count, VkPresentModeKHR* modes) {
  const auto function = host_function<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
  if (!function || !count) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = function(physical_device, surface, count, modes);
  if (result != VK_SUCCESS || !modes || *count == 0 || requested_present_mode() == RequestedPresentMode::none) return result;
  const VkPresentModeKHR wanted = requested_present_mode_value();
  uint32_t wanted_index = find_present_mode_index(modes, *count, wanted);
  const char* mode_name = requested_present_mode_name();
  if (wanted_index == *count) {
    // FIFO_RELAXED often absent on Wayland; leaving the full list lets clients pick IMMEDIATE and spike cadence. Prefer FIFO; keep driver list only if FIFO missing.
    uint32_t fifo_index = find_present_mode_index(modes, *count, VK_PRESENT_MODE_FIFO_KHR);
    if (fifo_index == *count) {
      trace_present_mode(mode_name, false);
      return result;
    }
    if (bootstrap_trace_enabled()) std::fprintf(stderr, "nuah vulkan: requested present mode=%s unavailable; falling back to fifo\n", mode_name);
    wanted_index = fifo_index;
  }
  // Expose only the requested mode; moving FIFO first is insufficient for IMMEDIATE-preferring clients.
  modes[0] = modes[wanted_index];
  *count = 1;
  trace_present_mode(mode_name, true);
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkInstance* output) {
  const auto function = host_function<PFN_vkCreateInstance>("vkCreateInstance");
  if (!function || !create_info || !output) return VK_ERROR_INITIALIZATION_FAILED;
  // Android ABI asks for VK_KHR_android_surface; Linux loader rejects it. Swap only that name for Wayland; adapter translates the surface later.
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
    if (std::strcmp(name, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0) has_wayland_surface = true;
    extensions.push_back(name);
  }
  if (requested_android_surface && !has_wayland_surface && host_has_instance_extension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
    has_wayland_surface = true;
  }
  VkInstanceCreateInfo forwarded = *create_info;
  if (requested_android_surface) {
    forwarded.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    forwarded.ppEnabledExtensionNames = extensions.data();
    if (bootstrap_trace_enabled()) std::fprintf(stderr, "nuah vulkan: replaced VK_KHR_android_surface with %s\n", has_wayland_surface ? VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME : "no host WSI extension");
  }
  return function(&forwarded, allocator, output);
}
VKAPI_ATTR VkResult VKAPI_CALL bionic_vkCreateInstance(VkInstanceCreateInfo* create_info, const VkAllocationCallbacks* allocator, VkInstance* output) {
  return vkCreateInstance(create_info, allocator, output);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL bionic_vkGetInstanceProcAddr(VkInstance instance, const char* name) { return vkGetInstanceProcAddr(instance, name); }
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
  const auto owned_surfaces = take_instance_surfaces(instance);
  for (const auto& [surface, window] : owned_surfaces) {
    SDL_Vulkan_DestroySurface(instance, surface, allocator);
    nuah_native_window_release(window);
  }
  const auto function = host_function<PFN_vkDestroyInstance>("vkDestroyInstance");
  if (function) function(instance, allocator);
}
#define INTERPOSE_IF(fn, cond) \
  if ((cond) && std::strcmp(name, #fn) == 0 && host_function<PFN_##fn>(#fn)) return reinterpret_cast<PFN_vkVoidFunction>(&fn)
#define INTERPOSE_STATIC(fn, cond) \
  if ((cond) && std::strcmp(name, #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&fn)
// X-macro lists: one list drives both device (IF) and instance (STATIC) interposition sites.
#define NUAH_FOR_EACH_CMD_LIFECYCLE(X) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkFreeCommandBuffers) X(vkCmdExecuteCommands)
#define NUAH_FOR_EACH_CMD_BIND(X) X(vkCmdBindPipeline) X(vkCmdBindIndexBuffer) X(vkCmdBindVertexBuffers) X(vkCmdBindVertexBuffers2)
#define NUAH_FOR_EACH_PIPELINE(X) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines)
#define NUAH_FOR_EACH_COPY_IMAGE(X) X(vkCmdCopyImage2) X(vkCmdCopyImage)
#define NUAH_EXPAND_IF_TRUE(fn) INTERPOSE_IF(fn, true);
#define NUAH_EXPAND_STATIC_TRUE(fn) INTERPOSE_STATIC(fn, true);
#define NUAH_EXPAND_PIPELINE_STATIC(fn) INTERPOSE_STATIC(fn, residency_enabled() || pipeline_trace_enabled());
#define NUAH_EXPAND_COPY_IF(fn) INTERPOSE_IF(fn, copy_trace_enabled() || engine_trace_enabled());
#define NUAH_CMD_LIFECYCLE_IF() NUAH_FOR_EACH_CMD_LIFECYCLE(NUAH_EXPAND_IF_TRUE)
#define NUAH_CMD_LIFECYCLE_STATIC() NUAH_FOR_EACH_CMD_LIFECYCLE(NUAH_EXPAND_STATIC_TRUE)
#define NUAH_CMD_BIND_IF() NUAH_FOR_EACH_CMD_BIND(NUAH_EXPAND_IF_TRUE)
#define NUAH_CMD_BIND_STATIC() NUAH_FOR_EACH_CMD_BIND(NUAH_EXPAND_STATIC_TRUE)
#define NUAH_PIPELINES_STATIC() NUAH_FOR_EACH_PIPELINE(NUAH_EXPAND_PIPELINE_STATIC)
#define NUAH_COPY_IMAGE_IF() NUAH_FOR_EACH_COPY_IMAGE(NUAH_EXPAND_COPY_IF)
}  // extern "C"
template <typename PFN_T, typename Props, typename Proj>
inline void fetch_mem_props(const char* name, VkPhysicalDevice pd, Props* out, Proj proj) {
  const auto fn = host_function<PFN_T>(name);
  if (!fn || !out) return;
  fn(pd, out);
  trace_memory_properties(pd, proj(*out));
}
extern "C" {
static inline VkResult normalize_result(VkResult result, VkSurfaceCapabilitiesKHR* caps, VkSurfaceKHR surface) {
  if (result == VK_SUCCESS) normalize_caps_and_release(caps, surface);
  return result;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
  if (!name) return nullptr;
  INTERPOSE_STATIC(vkQueuePresentKHR, perf_trace_enabled() || engine_trace_enabled() || frame_work_trace_enabled());
  NUAH_PIPELINES_STATIC();
  INTERPOSE_IF(vkCreatePipelineCache, residency_enabled()); INTERPOSE_IF(vkGetPipelineCacheData, residency_enabled()); INTERPOSE_IF(vkDestroyPipelineCache, residency_enabled());
  INTERPOSE_STATIC(vkQueueWaitIdle, wait_trace_enabled()); INTERPOSE_STATIC(vkDeviceWaitIdle, wait_trace_enabled() || residency_enabled());
  INTERPOSE_IF(vkDestroyDevice, residency_enabled());
  INTERPOSE_STATIC(vkWaitForFences, wait_trace_enabled() || engine_trace_enabled());
  INTERPOSE_IF(vkQueueSubmit, submit_trace_enabled() || engine_trace_enabled() || frame_work_trace_enabled() || perf_trace_enabled());
  INTERPOSE_IF(vkQueueSubmit2, submit_trace_enabled() || engine_trace_enabled() || frame_work_trace_enabled() || perf_trace_enabled());
  INTERPOSE_IF(vkCreateImage, engine_trace_enabled() || intel_format_trace_enabled() || texture_upload_trace_enabled() || frame_work_trace_enabled());
  INTERPOSE_IF(vkDestroyImage, texture_upload_trace_enabled() || frame_work_trace_enabled());
  INTERPOSE_IF(vkBindBufferMemory, texture_upload_hash_trace_enabled()); INTERPOSE_IF(vkDestroyBuffer, texture_upload_hash_trace_enabled());
  INTERPOSE_IF(vkMapMemory, texture_upload_hash_trace_enabled()); INTERPOSE_IF(vkUnmapMemory, texture_upload_hash_trace_enabled());
  INTERPOSE_IF(vkCmdCopyBufferToImage, copy_trace_enabled() || engine_trace_enabled() || texture_upload_trace_enabled() || frame_work_trace_enabled());
  INTERPOSE_IF(vkCmdCopyBufferToImage2, copy_trace_enabled() || engine_trace_enabled() || texture_upload_trace_enabled() || frame_work_trace_enabled());
  NUAH_COPY_IMAGE_IF();
  INTERPOSE_STATIC(vkAcquireNextImageKHR, wait_trace_enabled() || engine_trace_enabled());
  INTERPOSE_IF(vkAllocateDescriptorSets, engine_trace_enabled() || descriptor_alloc_batch_size()); INTERPOSE_IF(vkFreeDescriptorSets, engine_trace_enabled());
  INTERPOSE_IF(vkCreateDescriptorPool, engine_trace_enabled()); INTERPOSE_IF(vkUpdateDescriptorSets, engine_trace_enabled()); INTERPOSE_IF(vkCreateImageView, engine_trace_enabled());
  INTERPOSE_IF(vkResetDescriptorPool, engine_trace_enabled() || descriptor_alloc_batch_size()); INTERPOSE_IF(vkDestroyDescriptorPool, descriptor_alloc_batch_size());
  INTERPOSE_IF(vkCreateSampler, texture_min_lod() > 0.0f);
  if (descriptor_bind_dedup_enabled()) {
    NUAH_CMD_LIFECYCLE_IF();
  }
  if (command_state_dedup_enabled()) {
    NUAH_CMD_BIND_IF();
  }
  INTERPOSE_IF(vkCmdBindDescriptorSets, engine_trace_enabled() || descriptor_bind_dedup_enabled());
  INTERPOSE_IF(vkCmdPipelineBarrier, engine_trace_enabled());
  INTERPOSE_STATIC(vkGetPhysicalDeviceSurfacePresentModesKHR, true);
  const auto function = host_function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
  return function ? function(device, name) : nullptr;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physical_device, VkPhysicalDeviceMemoryProperties* memory_properties) {
  fetch_mem_props<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties", physical_device, memory_properties, [](const VkPhysicalDeviceMemoryProperties& p) -> const VkPhysicalDeviceMemoryProperties& { return p; });
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physical_device, VkPhysicalDeviceMemoryProperties2* memory_properties) {
  fetch_mem_props<PFN_vkGetPhysicalDeviceMemoryProperties2>("vkGetPhysicalDeviceMemoryProperties2", physical_device, memory_properties,
                                                            [](const VkPhysicalDeviceMemoryProperties2& p) -> const VkPhysicalDeviceMemoryProperties& { return p.memoryProperties; });
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info) {
  NUAH_VK_HOST(PFN_vkQueuePresentKHR, vkQueuePresentKHR, VK_ERROR_DEVICE_LOST);
  const bool record_metrics = perf_trace_enabled() || engine_trace_enabled();
  const bool record_frame_work = frame_work_trace_enabled();
  if (!record_metrics && !record_frame_work) return nuah_host_fn(queue, present_info);
  const uint64_t started_ns = monotonic_ns();
  const VkResult result = nuah_host_fn(queue, present_info);
  const uint64_t finished_ns = monotonic_ns();
  if (record_metrics) record_present(started_ns, finished_ns);
  if (record_frame_work) record_frame_present(finished_ns);
  return result;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  nuah_android_api_register("libvulkan.so", "vkGetInstanceProcAddr", NUAH_ANDROID_API_FORWARDED);
  if (!name) return nullptr;
  INTERPOSE_STATIC(vkCreateAndroidSurfaceKHR, true); INTERPOSE_STATIC(vkDestroySurfaceKHR, true); INTERPOSE_STATIC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, true);
  INTERPOSE_STATIC(vkGetPhysicalDeviceSurfaceCapabilities2KHR, true); INTERPOSE_STATIC(vkGetPhysicalDeviceSurfacePresentModesKHR, true);
  INTERPOSE_IF(vkGetPhysicalDeviceMemoryProperties, true); INTERPOSE_IF(vkGetPhysicalDeviceMemoryProperties2, true);
  INTERPOSE_STATIC(vkCreatePipelineCache, residency_enabled()); INTERPOSE_STATIC(vkGetPipelineCacheData, residency_enabled()); INTERPOSE_STATIC(vkDestroyPipelineCache, residency_enabled());
  NUAH_PIPELINES_STATIC();
  INTERPOSE_STATIC(vkQueuePresentKHR, perf_trace_enabled());
  NUAH_COPY_IMAGE_IF();
  if (descriptor_bind_dedup_enabled()) {
    NUAH_CMD_LIFECYCLE_STATIC();
    INTERPOSE_STATIC(vkCmdBindDescriptorSets, true);
  }
  if (descriptor_alloc_batch_size()) {
    INTERPOSE_STATIC(vkAllocateDescriptorSets, true); INTERPOSE_STATIC(vkResetDescriptorPool, true); INTERPOSE_STATIC(vkDestroyDescriptorPool, true);
  }
  if (command_state_dedup_enabled()) {
    NUAH_CMD_BIND_STATIC();
  }
  INTERPOSE_STATIC(vkDestroyDevice, residency_enabled());
  INTERPOSE_STATIC(vkGetInstanceProcAddr, true);
  const auto function = host_function<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
  return function ? function(instance, name) : nullptr;
}
}  // extern "C"
