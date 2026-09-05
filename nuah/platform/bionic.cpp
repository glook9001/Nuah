#include <dlfcn.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csetjmp>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "nuah/bootstrap_diagnostics.h"
#define NUAH_STRINGIFY_INNER(value) #value
#define NUAH_STRINGIFY(value) NUAH_STRINGIFY_INNER(value)
#ifndef NUAH_LIKELY
#define NUAH_LIKELY(x) (__builtin_expect(!!(x), 1))
#endif
#ifndef NUAH_UNLIKELY
#define NUAH_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif
extern "C" {
// Legacy Android objects still reference Bionic's pre-API-23 __sF array.
// Bionic's LP64 FILE ABI is an opaque 152-byte object; it is not glibc FILE.
alignas(void*) unsigned char __sF[3][152]{};
uintptr_t __stack_chk_guard = 0x9e3779b97f4a7c15ULL;
std::FILE* nuah_stdin __asm__("stdin") = nullptr;
std::FILE* nuah_stdout __asm__("stdout") = nullptr;
std::FILE* nuah_stderr __asm__("stderr") = nullptr;
int nuah_daylight __asm__("daylight") = 0;
long nuah_timezone __asm__("timezone") = 0;
char* nuah_tzname[2] __asm__("tzname") = {nullptr, nullptr};
char** nuah_environ __asm__("environ") = nullptr;
in6_addr nuah_in6addr_any __asm__("in6addr_any"){};
in6_addr nuah_in6addr_loopback __asm__("in6addr_loopback"){};
in6_addr nuah_in6addr_any_n{};
in6_addr nuah_in6addr_loopback_n{};
char* nuah_optarg __asm__("optarg") = nullptr;
int nuah_optind __asm__("optind") = 1;
asm(".symver nuah_in6addr_any_n,in6addr_any@LIBC_N");
asm(".symver nuah_in6addr_loopback_n,in6addr_loopback@LIBC_N");
/* glibc publishes the internal pthread entry points with fixed symbol
 * versions.  Binding these at link time avoids calling dlsym from a TLS
 * function (the loader itself asks for TLS while resolving dlsym). */
int nuah_host_pthread_once(pthread_once_t*, void (*)(void)); int nuah_host_pthread_key_create(pthread_key_t*, void (*)(void*)); int nuah_host_pthread_key_delete(pthread_key_t); void* nuah_host_pthread_getspecific(pthread_key_t); int nuah_host_pthread_setspecific(pthread_key_t, const void*);
int nuah_host_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*); int nuah_host_mutex_destroy(pthread_mutex_t*); int nuah_host_mutex_lock(pthread_mutex_t*); int nuah_host_mutex_trylock(pthread_mutex_t*); int nuah_host_mutex_unlock(pthread_mutex_t*); int nuah_host_mutexattr_init(pthread_mutexattr_t*); int nuah_host_mutexattr_destroy(pthread_mutexattr_t*); int nuah_host_mutexattr_settype(pthread_mutexattr_t*, int);
int nuah_host_cond_init(pthread_cond_t*, const pthread_condattr_t*); int nuah_host_cond_destroy(pthread_cond_t*); int nuah_host_cond_signal(pthread_cond_t*); int nuah_host_cond_broadcast(pthread_cond_t*); int nuah_host_cond_wait(pthread_cond_t*, pthread_mutex_t*); int nuah_host_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const timespec*);
int nuah_host_rwlock_init(pthread_rwlock_t*, const pthread_rwlockattr_t*); int nuah_host_rwlock_destroy(pthread_rwlock_t*); int nuah_host_rwlock_rdlock(pthread_rwlock_t*); int nuah_host_rwlock_wrlock(pthread_rwlock_t*); int nuah_host_rwlock_unlock(pthread_rwlock_t*);
int nuah_host_getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo**);
#define NUAH_SYMVER3(h, s) asm(".symver " #h "," #s "@GLIBC_2.2.5")
NUAH_SYMVER3(nuah_host_pthread_once, __pthread_once); NUAH_SYMVER3(nuah_host_pthread_key_create, __pthread_key_create); NUAH_SYMVER3(nuah_host_pthread_key_delete, pthread_key_delete); NUAH_SYMVER3(nuah_host_pthread_getspecific, __pthread_getspecific); NUAH_SYMVER3(nuah_host_pthread_setspecific, __pthread_setspecific);
NUAH_SYMVER3(nuah_host_mutex_init, __pthread_mutex_init); NUAH_SYMVER3(nuah_host_mutex_destroy, __pthread_mutex_destroy); NUAH_SYMVER3(nuah_host_mutex_lock, __pthread_mutex_lock); NUAH_SYMVER3(nuah_host_mutex_trylock, __pthread_mutex_trylock); NUAH_SYMVER3(nuah_host_mutex_unlock, __pthread_mutex_unlock);
NUAH_SYMVER3(nuah_host_mutexattr_init, pthread_mutexattr_init); NUAH_SYMVER3(nuah_host_mutexattr_destroy, pthread_mutexattr_destroy); NUAH_SYMVER3(nuah_host_mutexattr_settype, pthread_mutexattr_settype); NUAH_SYMVER3(nuah_host_cond_init, pthread_cond_init); NUAH_SYMVER3(nuah_host_cond_destroy, pthread_cond_destroy); NUAH_SYMVER3(nuah_host_cond_signal, pthread_cond_signal);
NUAH_SYMVER3(nuah_host_cond_broadcast, pthread_cond_broadcast); NUAH_SYMVER3(nuah_host_cond_wait, pthread_cond_wait); NUAH_SYMVER3(nuah_host_cond_timedwait, pthread_cond_timedwait); NUAH_SYMVER3(nuah_host_rwlock_init, __pthread_rwlock_init); NUAH_SYMVER3(nuah_host_rwlock_destroy, __pthread_rwlock_destroy); NUAH_SYMVER3(nuah_host_rwlock_rdlock, __pthread_rwlock_rdlock);
NUAH_SYMVER3(nuah_host_rwlock_wrlock, __pthread_rwlock_wrlock); NUAH_SYMVER3(nuah_host_rwlock_unlock, __pthread_rwlock_unlock); NUAH_SYMVER3(nuah_host_getaddrinfo, getaddrinfo);
}
namespace {
NuahDiagnosticsCallbacks diagnostics_callbacks{};
/* Android's pthread_key_t values live in a namespace owned by the Android
 * image.  Passing them straight to glibc is normally harmless until another
 * provider has already consumed the same small key number: Roblox then reads
 * an unrelated host TLS object as its allocator arena.  Keep the historical
 * direct path by default, but offer an opt-in private key range for images
 * whose constructors depend on Android key identity. */
constexpr pthread_key_t kAndroidKeyBase = static_cast<pthread_key_t>(0x10000);
constexpr std::size_t kAndroidKeySlots = 256;
struct AndroidKeySlot {
  std::atomic<pthread_key_t> host_key{0};
  std::atomic<bool> used{false};
};
std::array<AndroidKeySlot, kAndroidKeySlots> android_key_slots{};
/* Some stripped Android images persist a small key number in static state
 * instead of retaining the result of pthread_key_create().  On glibc those
 * low numbers may already belong to Nuah/SDL/GLib, so lazily give each such
 * Android key its own host TLS slot when the namespace is enabled. */
std::array<AndroidKeySlot, kAndroidKeySlots> android_raw_key_slots{};
bool android_key_namespace_enabled();
pthread_key_t host_key_for_android(pthread_key_t key) {
  if (!android_key_namespace_enabled()) return key;
  if (key < kAndroidKeyBase) {
    if (key >= kAndroidKeySlots) return key;
    auto& slot = android_raw_key_slots[static_cast<std::size_t>(key)];
    if (!slot.used.load(std::memory_order_acquire)) {
      pthread_key_t host_key = 0;
      if (nuah_host_pthread_key_create(&host_key, nullptr) != 0) return key;
      bool expected = false;
      if (!slot.used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        (void)nuah_host_pthread_key_delete(host_key);
      } else {
        slot.host_key.store(host_key, std::memory_order_release);
      }
    }
    if (slot.used.load(std::memory_order_acquire)) return slot.host_key.load(std::memory_order_acquire);
    return key;
  }
  if (key - kAndroidKeyBase >= kAndroidKeySlots) return key;
  const auto index = static_cast<std::size_t>(key - kAndroidKeyBase);
  if (!android_key_slots[index].used.load(std::memory_order_acquire)) return key;
  return android_key_slots[index].host_key.load(std::memory_order_acquire);
}
template <typename T>
T host(const char* name);
/* Android's libcore/OkHttp path does not consistently honor
 * java.net.preferIPv4Stack on a host with no usable IPv6 route: it still
 * receives AAAA records and waits through each connect timeout before trying
 * the working A record.  Keep the workaround at the Bionic ABI edge.  Only
 * AF_UNSPEC lookups are narrowed; callers explicitly requesting AF_INET6
 * retain normal behavior, and getaddrinfo/freeaddrinfo still use glibc's
 * allocator pair. */
extern "C" int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** result) {
  static const bool prefer_ipv4_default = [] {
    const char* prefer_ipv4 = std::getenv("NUAH_PREFER_IPV4");
    return !prefer_ipv4 || std::strcmp(prefer_ipv4, "0") != 0;
  }();
  if (!prefer_ipv4_default || (hints && hints->ai_family != AF_UNSPEC)) return nuah_host_getaddrinfo(node, service, hints, result);
  struct addrinfo ipv4_hints = hints ? *hints : addrinfo{};
  ipv4_hints.ai_family = AF_INET;
  return nuah_host_getaddrinfo(node, service, &ipv4_hints, result);
}
enum class EntryState : unsigned char { empty, initializing, ready, failed, };
struct MutexAttrEntry { void* android = nullptr; int type = PTHREAD_MUTEX_NORMAL; };
template <typename Native>
struct SyncEntry { void* android = nullptr; Native native{}; std::atomic<EntryState> state{EntryState::empty}; };
using MutexEntry = SyncEntry<pthread_mutex_t>;
using CondEntry = SyncEntry<pthread_cond_t>;
using RwlockEntry = SyncEntry<pthread_rwlock_t>;
using SemEntry = SyncEntry<sem_t>;
struct OnceEntry { void* android = nullptr; pthread_once_t native = PTHREAD_ONCE_INIT; std::atomic<EntryState> state{EntryState::empty}; };
struct AttrEntry { void* android = nullptr; pthread_attr_t native{}; std::atomic<EntryState> state{EntryState::empty}; };
std::atomic_flag table_lock = ATOMIC_FLAG_INIT;
MutexEntry mutexes[2048];
/* Mutex lock/unlock is the hottest ABI edge in the Roblox renderer.  The
 * ownership table below remains the source of truth for creation and failure
 * handling, but looking up every ready mutex by taking one global spin lock
 * and scanning 2048 entries made ordinary engine synchronization needlessly
 * expensive.  This direct cache is best-effort: collisions simply fall back
 * to the authoritative table and never alter mutex semantics. */
struct MutexCacheEntry { std::atomic<void*> android{nullptr}; std::atomic<MutexEntry*> entry{nullptr}; };
constexpr std::size_t kMutexCacheCount = 4096;
MutexCacheEntry mutex_cache[kMutexCacheCount];
MutexAttrEntry mutex_attributes[256]; CondEntry conditions[1024]; OnceEntry onces[1024]; AttrEntry attributes[256]; RwlockEntry rwlocks[1024]; SemEntry semaphores[512];
/* Keep synchronization diagnostics opt-in and bounded.  These wrappers are
 * entered from Roblox's constructors, so an unbounded fprintf here can hide
 * the fault (or become recursive through libc).  The trace is deliberately a
 * probe, not a second synchronization implementation. */
std::atomic<unsigned> sync_trace_events{0};
std::atomic<unsigned> tls_trace_events{0};
#define NUAH_ENV_FLAG(fn, name)                        \
  bool fn() {                                          \
    static const bool v = [] {                         \
      const char* e = ::getenv(name);                  \
      return e && *e && std::strcmp(e, "0") != 0;      \
    }();                                               \
    return v;                                          \
  }
#define NUAH_TRACE_BUDGET(fn, flag, events, cap) \
  bool fn() {                                    \
    if (!flag()) return false;                   \
    return events.fetch_add(1, std::memory_order_relaxed) < cap; \
  }
NUAH_ENV_FLAG(android_key_namespace_enabled, "NUAH_PTHREAD_NAMESPACE") NUAH_ENV_FLAG(sync_trace_enabled, "NUAH_TRACE_PTHREAD") NUAH_ENV_FLAG(tls_trace_enabled, "NUAH_TRACE_TLS")
NUAH_ENV_FLAG(engine_trace_enabled, "NUAH_ENGINE_TRACE") NUAH_ENV_FLAG(pthread_tls_guard_enabled, "NUAH_PTHREAD_TLS_GUARD") NUAH_ENV_FLAG(bootstrap_trace_enabled, "NUAH_BOOTSTRAP_TRACE")
NUAH_TRACE_BUDGET(sync_trace_slot, sync_trace_enabled, sync_trace_events, 4096) NUAH_TRACE_BUDGET(tls_trace_slot, tls_trace_enabled, tls_trace_events, 1024)
uint64_t monotonic_ns() { timespec value{}; (void)::clock_gettime(CLOCK_MONOTONIC, &value); return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL + static_cast<uint64_t>(value.tv_nsec); }
struct SyncAggregate { const char* kind = nullptr; uintptr_t offset = 0; uint64_t calls = 0, contended = 0, total_ns = 0, max_ns = 0; };
struct SyncTrace { std::atomic_flag lock = ATOMIC_FLAG_INIT; std::array<SyncAggregate, 32> entries{}; uint64_t next_report_ns = 0; };
SyncTrace& sync_trace() { static SyncTrace value; return value; }
void record_sync_wait(const char* kind, uint64_t started_ns, const void* return_address) {
  if (!engine_trace_enabled()) return;
  const uint64_t elapsed_ns = monotonic_ns() - started_ns;
  Dl_info caller{};
  uintptr_t offset = reinterpret_cast<uintptr_t>(return_address);
  if (::dladdr(return_address, &caller) != 0 && caller.dli_fbase) offset -= reinterpret_cast<uintptr_t>(caller.dli_fbase);
  SyncTrace& trace = sync_trace();
  if (trace.lock.test_and_set(std::memory_order_acquire)) return;
  std::array<SyncAggregate, 32> report{};
  bool ready = false;
  for (auto& entry : trace.entries) {
    if (entry.kind == kind && entry.offset == offset) {
      ++entry.calls;
      entry.contended += elapsed_ns >= 50000ULL;
      entry.total_ns += elapsed_ns;
      entry.max_ns = std::max(entry.max_ns, elapsed_ns);
      break;
    }
    if (!entry.kind) {
      entry.kind = kind;
      entry.offset = offset;
      entry.calls = 1;
      entry.contended = elapsed_ns >= 50000ULL;
      entry.total_ns = elapsed_ns;
      entry.max_ns = elapsed_ns;
      break;
    }
  }
  const uint64_t now = monotonic_ns();
  if (trace.next_report_ns == 0) trace.next_report_ns = now + 1000000000ULL;
  if (now >= trace.next_report_ns) {
    report = trace.entries;
    trace.entries = {};
    trace.next_report_ns = now + 1000000000ULL;
    ready = true;
  }
  trace.lock.clear(std::memory_order_release);
  if (!ready) return;
  for (const auto& entry : report) {
    if (!entry.kind || !entry.calls) continue;
    std::fprintf(stderr, "nuah engine: sync=%s caller_offset=0x%llx calls=%llu contended=%llu avg_us=%llu max_us=%llu\n", entry.kind, static_cast<unsigned long long>(entry.offset),
                 static_cast<unsigned long long>(entry.calls), static_cast<unsigned long long>(entry.contended), static_cast<unsigned long long>(entry.total_ns / entry.calls / 1000ULL),
                 static_cast<unsigned long long>(entry.max_ns / 1000ULL));
  }
}
// Android 16 x86-64 exposes an 88-byte jmp_buf; glibc uses a 200-byte buffer.
// Key stable host buffers by the opaque Android address instead of writing a
// host object past the caller's allocation. setjmp itself is an assembly
// tail-call (bionic_setjmp.S), so the saved stack belongs to Roblox's caller.
struct JumpEntry { std::atomic<void*> android{nullptr}; std::jmp_buf native{}; };
JumpEntry jump_buffers[4096];
constexpr std::size_t kJumpBufferCount = sizeof(jump_buffers) / sizeof(jump_buffers[0]);
using AndroidSignalHandler = void (*)(int);
using AndroidSignalAction = void (*)(int, siginfo_t*, void*);
struct AndroidSigaction {
  int flags;
  union {
    AndroidSignalHandler handler;
    AndroidSignalAction action;
  };
  unsigned long mask;
  void (*restorer)(void);
};
void lock_table() { while (table_lock.test_and_set(std::memory_order_acquire)) {} }
void unlock_table() { table_lock.clear(std::memory_order_release); }
template <typename T> T host(const char* name) { return reinterpret_cast<T>(::dlsym(RTLD_NEXT, name)); }
std::size_t mutex_cache_index(void* object) { const uintptr_t address = reinterpret_cast<uintptr_t>(object); return (address >> 3U) & (kMutexCacheCount - 1U); }
MutexEntry* cached_mutex(void* object) {
  if (!object) return nullptr;
  MutexCacheEntry& cache = mutex_cache[mutex_cache_index(object)];
  if (cache.android.load(std::memory_order_acquire) != object) return nullptr;
  MutexEntry* entry = cache.entry.load(std::memory_order_acquire);
  if (!entry || entry->state.load(std::memory_order_acquire) != EntryState::ready) return nullptr;
  return entry;
}
void cache_mutex(void* object, MutexEntry* entry) {
  if (!object || !entry) return;
  MutexCacheEntry& cache = mutex_cache[mutex_cache_index(object)];
  void* expected = nullptr;
  if (cache.android.compare_exchange_strong(expected, object, std::memory_order_acq_rel) || expected == object) { cache.entry.store(entry, std::memory_order_release); }
}
constexpr unsigned kEntryWaitSpins = 4096;
template <typename Entry> bool wait_for_entry(Entry& entry, void* object) {
  for (unsigned attempt = 0; attempt < kEntryWaitSpins; ++attempt) {
    const auto state = entry.state.load(std::memory_order_acquire);
    if (state == EntryState::ready) return true;
    if (state == EntryState::failed || state == EntryState::empty) return false;
    if ((attempt & 31U) == 0) (void)::sched_yield();
  }
  if (sync_trace_slot()) std::fprintf(stderr, "nuah bionic: synchronization entry initialization timed out android=%p\n", object);
  return false;
}
template <typename Entry, std::size_t Count> Entry* scan_locked(Entry (&entries)[Count], void* object) {
  for (auto& entry : entries) if (entry.android == object) return &entry;
  return nullptr;
}
// Shared failed-slot reclamation: returns true when the caller must fall through to reservation/destroy handling.
template <typename Entry> bool reclaim_failed_locked(Entry& entry) {
  if (entry.state.load(std::memory_order_relaxed) != EntryState::failed) return false;
  entry.android = nullptr;
  entry.state.store(EntryState::empty, std::memory_order_relaxed);
  return true;
}
template <typename Entry, std::size_t Count> Entry* find_ready(Entry (&entries)[Count], void* object) {
  lock_table();
  if (auto* found = scan_locked(entries, object)) {
    if (!reclaim_failed_locked(*found)) { unlock_table(); return wait_for_entry(*found, object) ? found : nullptr; }
  }
  unlock_table();
  return nullptr;
}
template <typename Entry, std::size_t Count> Entry* find_or_reserve(Entry (&entries)[Count], void* object, bool& initialize) {
  initialize = false;
  lock_table();
  if (auto* found = scan_locked(entries, object)) {
    if (!reclaim_failed_locked(*found)) { unlock_table(); return wait_for_entry(*found, object) ? found : nullptr; }
  }
  for (auto& entry : entries) {
    if (entry.android == nullptr && entry.state.load(std::memory_order_relaxed) == EntryState::empty) {
      entry.android = object;
      entry.state.store(EntryState::initializing, std::memory_order_release);
      initialize = true;
      unlock_table();
      return &entry;
    }
  }
  unlock_table();
  return nullptr;
}
template <typename Entry>
void publish_entry(Entry& entry, bool success) { entry.state.store(success ? EntryState::ready : EntryState::failed, std::memory_order_release); }
template <typename Entry>
void clear_entry(Entry& entry, void* object) {
  lock_table();
  if (entry.android == object) { entry.android = nullptr; entry.state.store(EntryState::empty, std::memory_order_release); }
  unlock_table();
}
template <typename Entry, std::size_t Count> Entry* begin_destroy(Entry (&entries)[Count], void* object) {
  lock_table();
  if (auto* found = scan_locked(entries, object)) {
    if (found->state.load(std::memory_order_relaxed) != EntryState::ready) { unlock_table(); return nullptr; }
    /* Keep the stable slot reserved while the external destroy call runs.
     * New users will wait and observe an empty slot after successful
     * destruction instead of touching a half-destroyed host object. */
    found->state.store(EntryState::initializing, std::memory_order_release);
    unlock_table();
    return found;
  }
  unlock_table();
  return nullptr;
}
std::FILE* host_stream(std::FILE* stream) {
  const auto address = reinterpret_cast<uintptr_t>(stream), base = reinterpret_cast<uintptr_t>(__sF);
  return address == base ? nuah_stdin : address == base + sizeof(__sF[0]) ? nuah_stdout : address == base + 2 * sizeof(__sF[0]) ? nuah_stderr : stream;
}
extern "C" void* nuah_host_jmpbuf(void* android_buffer) {
  if (!android_buffer) std::abort();
  const auto key = reinterpret_cast<uintptr_t>(android_buffer);
  const std::size_t start = (key >> 3U) % kJumpBufferCount;
  for (std::size_t offset = 0; offset < kJumpBufferCount; ++offset) {
    auto& entry = jump_buffers[(start + offset) % kJumpBufferCount];
    void* existing = entry.android.load(std::memory_order_acquire);
    if (existing == android_buffer) return static_cast<void*>(entry.native);
    if (!existing) {
      void* expected = nullptr;
      if (entry.android.compare_exchange_strong(expected, android_buffer, std::memory_order_acq_rel)) { return static_cast<void*>(entry.native); }
      if (expected == android_buffer) return static_cast<void*>(entry.native);
    }
  }
  std::fprintf(stderr, "nuah bootstrap: exhausted translated Android jmp_buf table\n");
  std::abort();
}
int host_signal_number(int signal_number) {
  // Android reserves 33 for its thread/backtrace signal. glibc reserves its
  // own internal pair below SIGRTMIN, so route that Android-visible slot to
  // the first host real-time signal as the established compatibility layers
  // in this tree do.
  return signal_number == 33 ? SIGRTMIN : signal_number;
}
MutexAttrEntry* find_mutex_attr_locked(void* object) { for (auto& entry : mutex_attributes) if (entry.android == object) return &entry;
  return nullptr; }
int mutex_attr_type(const pthread_mutexattr_t* object) {
  if (!object) return PTHREAD_MUTEX_NORMAL;
  lock_table();
  auto* found = find_mutex_attr_locked(const_cast<void*>(static_cast<const void*>(object)));
  const int type = found ? found->type : PTHREAD_MUTEX_NORMAL;
  unlock_table();
  return type;
}
MutexAttrEntry* mutex_attr_for(void* object) {
  if (!object) return nullptr;
  lock_table();
  if (auto* found = find_mutex_attr_locked(object)) { unlock_table(); return found; }
  for (auto& entry : mutex_attributes) {
    if (!entry.android) { entry.android = object; entry.type = PTHREAD_MUTEX_NORMAL; unlock_table(); return &entry; }
  }
  unlock_table();
  return nullptr;
}
MutexEntry* mutex_for(void* object, const pthread_mutexattr_t* attributes = nullptr) {
  if (!attributes) {
    if (auto* entry = cached_mutex(object)) return entry;
  }
  const int requested_type = attributes ? mutex_attr_type(attributes) : PTHREAD_MUTEX_NORMAL;
  bool initialize = false;
  auto* entry = find_or_reserve(mutexes, object, initialize);
  if (!entry) return nullptr;
  if (!initialize) {
    cache_mutex(object, entry);
    return entry;
  }
  /* Android's static mutex initializer stores its type in bits 14..15,
   * while glibc's object layout is unrelated.  Preserve that type before
   * creating the out-of-line host object; Roblox uses recursive static
   * mutexes in its TLS singleton and a normal host mutex deadlocks there. */
  std::uint32_t initializer = 0;
  if (object) std::memcpy(&initializer, object, sizeof(initializer));
  int type = requested_type;
  if (!attributes) {
    switch ((initializer >> 14U) & 0x3U) {
      case 1:
        type = PTHREAD_MUTEX_RECURSIVE;
        break;
      case 2:
        type = PTHREAD_MUTEX_ERRORCHECK;
        break;
      default:
        break;
    }
  }
  pthread_mutexattr_t host_attributes{};
  bool success = false;
  if (nuah_host_mutexattr_init(&host_attributes) == 0) {
    (void)nuah_host_mutexattr_settype(&host_attributes, type);
    success = nuah_host_mutex_init(&entry->native, &host_attributes) == 0;
    (void)nuah_host_mutexattr_destroy(&host_attributes);
  } else {
    success = nuah_host_mutex_init(&entry->native, nullptr) == 0;
  }
  publish_entry(*entry, success);
  if (success) cache_mutex(object, entry);
  return success ? entry : nullptr;
}
template <typename Entry, std::size_t Count, typename Init>
Entry* init_entry(Entry (&table)[Count], void* object, Init init) {
  bool initialize = false;
  auto* entry = find_or_reserve(table, object, initialize);
  if (!entry || !initialize) return entry;
  const bool success = init(entry->native);
  publish_entry(*entry, success);
  return success ? entry : nullptr;
}
CondEntry* cond_for(void* object) { return init_entry(conditions, object, [](pthread_cond_t& native) { return nuah_host_cond_init(&native, nullptr) == 0; }); }
OnceEntry* once_for(void* object) { return init_entry(onces, object, [](pthread_once_t& native) { native = PTHREAD_ONCE_INIT; return true; }); }
AttrEntry* attr_for(void* object) {
  if (!object) return nullptr;
  return init_entry(attributes, object, [](pthread_attr_t& native) { auto init = host<int (*)(pthread_attr_t*)>("pthread_attr_init"); return init && init(&native) == 0; });
}
RwlockEntry* rwlock_for(void* object) {
  if (!object) return nullptr;
  return init_entry(rwlocks, object, [](pthread_rwlock_t& native) { return nuah_host_rwlock_init(&native, nullptr) == 0; });
}
SemEntry* sem_for(void* object) { return find_ready(semaphores, object); }
}  // namespace
extern "C" [[noreturn]] void abort() {
  if (diagnostics_callbacks.record_abort) { diagnostics_callbacks.record_abort(__builtin_return_address(0)); }
  const auto host_abort = reinterpret_cast<void (*)()>(::dlvsym(RTLD_DEFAULT, "abort", "GLIBC_2.2.5"));
  if (host_abort && reinterpret_cast<void*>(host_abort) != reinterpret_cast<void*>(abort)) { host_abort(); }
  ::raise(SIGABRT);
  ::_exit(128 + SIGABRT);
}
extern "C" void nuah_bionic_set_diagnostics_callbacks(const NuahDiagnosticsCallbacks* callbacks) {
  diagnostics_callbacks = callbacks && callbacks->version == 1 ? *callbacks : NuahDiagnosticsCallbacks{};
}
void synchronize_timezone_data() {
  if (auto* value = host<int*>("daylight")) nuah_daylight = *value;
  if (auto* value = host<long*>("timezone")) nuah_timezone = *value;
  if (auto** value = host<char**>("tzname")) { nuah_tzname[0] = value[0]; nuah_tzname[1] = value[1]; }
}
extern "C" {
#define NUAH_SIGSET_NULLARY(fn, asm_name, assign) int fn(unsigned long* set) __asm__(asm_name); int fn(unsigned long* set) { if (!set) { errno = EINVAL; return -1; } *set = (assign); return 0; }
NUAH_SIGSET_NULLARY(nuah_sigemptyset, "sigemptyset", 0) NUAH_SIGSET_NULLARY(nuah_sigfillset, "sigfillset", ~0UL)
int nuah_sigaddset(unsigned long* set, int signal_number) __asm__("sigaddset"); int nuah_sigaddset(unsigned long* set, int signal_number) { if (!set || signal_number < 1 || signal_number > 64) { errno = EINVAL; return -1; } *set |= 1UL << (signal_number - 1); return 0; }
int nuah_sigaction(int signal_number, const AndroidSigaction* action, AndroidSigaction* old_action) __asm__("sigaction");
int nuah_sigaction(int signal_number, const AndroidSigaction* action, AndroidSigaction* old_action) {
  struct ::sigaction native_action{};
  struct ::sigaction native_old_action{};
  if (action) {
    native_action.sa_flags = action->flags;
    native_action.sa_restorer = action->restorer;
    if ((action->flags & SA_SIGINFO) != 0) {
      native_action.sa_sigaction = action->action;
    } else {
      native_action.sa_handler = action->handler;
    }
    host<int (*)(sigset_t*)>("sigemptyset")(&native_action.sa_mask);
    for (int candidate = 1; candidate <= 64; ++candidate) {
      if ((action->mask & (1UL << (candidate - 1))) != 0) { (void)host<int (*)(sigset_t*, int)>("sigaddset")(&native_action.sa_mask, host_signal_number(candidate)); }
    }
  }
  const int result =
      host<int (*)(int, const struct ::sigaction*, struct ::sigaction*)>("sigaction")(host_signal_number(signal_number), action ? &native_action : nullptr, old_action ? &native_old_action : nullptr);
  if (result != 0 || !old_action) return result;
  std::memset(old_action, 0, sizeof(*old_action));
  old_action->flags = native_old_action.sa_flags;
  old_action->restorer = native_old_action.sa_restorer;
  if ((native_old_action.sa_flags & SA_SIGINFO) != 0) {
    old_action->action = native_old_action.sa_sigaction;
  } else {
    old_action->handler = native_old_action.sa_handler;
  }
  for (int candidate = 1; candidate <= 64; ++candidate) {
    if (host<int (*)(const sigset_t*, int)>("sigismember")(&native_old_action.sa_mask, host_signal_number(candidate)) == 1) { old_action->mask |= 1UL << (candidate - 1); }
  }
  return result;
}
int fflush(std::FILE* s) { return host<int (*)(std::FILE*)>("fflush")(s ? host_stream(s) : nullptr); }
size_t fread(void* d, size_t s, size_t c, std::FILE* st) { return host<size_t (*)(void*, size_t, size_t, std::FILE*)>("fread")(d, s, c, host_stream(st)); }
size_t fwrite(const void* d, size_t s, size_t c, std::FILE* st) { return host<size_t (*)(const void*, size_t, size_t, std::FILE*)>("fwrite")(d, s, c, host_stream(st)); }
int fclose(std::FILE* stream) {
  // Standard streams are process-owned and must not be closed by a legacy
  // Android FILE façade.
  std::FILE* translated = host_stream(stream);
  if (translated == nuah_stdin || translated == nuah_stdout || translated == nuah_stderr) { return 0; }
  static const auto fn = host<int (*)(std::FILE*)>("fclose");
  return fn(translated);
}
}
__attribute__((constructor)) static void initialize_standard_streams() {
  /* Seed guard from AT_RANDOM for API-36 ABI compat. */
  if (const auto random = ::getauxval(AT_RANDOM); random != 0) { std::memcpy(&__stack_chk_guard, reinterpret_cast<const void*>(random), sizeof(__stack_chk_guard)); }
  if (auto** value = host<std::FILE**>("stdin")) nuah_stdin = *value;
  if (auto** value = host<std::FILE**>("stdout")) nuah_stdout = *value;
  if (auto** value = host<std::FILE**>("stderr")) nuah_stderr = *value;
  if (auto*** value = host<char***>("environ")) nuah_environ = *value;
  if (auto* value = host<in6_addr*>("in6addr_any")) { nuah_in6addr_any = *value; nuah_in6addr_any_n = *value; }
  if (auto* value = host<in6_addr*>("in6addr_loopback")) { nuah_in6addr_loopback = *value; nuah_in6addr_loopback_n = *value; }
  synchronize_timezone_data();
}
extern "C" {
#define NUAH_FORTIFY(ret, name, params, size_expr, call) \
  ret name params {                                      \
    if (NUAH_UNLIKELY(size_expr)) std::abort();          \
    return call;                                         \
  }
NUAH_FORTIFY(void*, __memset_chk, (void* d, int v, size_t l, size_t s), l > s, memset(d, v, l)) NUAH_FORTIFY(void*, __memcpy_chk, (void* d, const void* src, size_t l, size_t s), l > s, memcpy(d, src, l)) NUAH_FORTIFY(void*, __memmove_chk, (void* d, const void* src, size_t l, size_t s), l > s, memmove(d, src, l))
NUAH_FORTIFY(char*, __strcpy_chk, (char* d, const char* src, size_t s), strlen(src) + 1 > s, strcpy(d, src)) NUAH_FORTIFY(char*, __strcat_chk, (char* d, const char* src, size_t s), strlen(d) + strlen(src) + 1 > s, strcat(d, src))
char* strerror(int e) { static const auto fn = host<char* (*)(int)>("strerror"); return fn(e); }
extern "C" int nuah_strerror_r(int e, char* b, size_t l) __asm__("strerror_r");
int nuah_strerror_r(int e, char* b, size_t l) {
  static const auto fn = host<int (*)(int, char*, size_t)>("__xpg_strerror_r");
  return fn(e, b, l);
}
extern "C" void nuah_tzset() __asm__("tzset");
void nuah_tzset() {
  static const auto fn = host<void (*)()>("tzset");
  fn();
  synchronize_timezone_data();
}
extern "C" int nuah_getopt_long(int argument_count, char* const arguments[], const char* options, const option* long_options, int* index) __asm__("getopt_long");
int nuah_getopt_long(int argument_count, char* const arguments[], const char* options, const option* long_options, int* index) {
  auto** host_optarg = host<char**>("optarg");
  auto* host_optind = host<int*>("optind");
  if (host_optarg) *host_optarg = nuah_optarg;
  if (host_optind) *host_optind = nuah_optind;
  const int result = host<int (*)(int, char* const[], const char*, const option*, int*)>("getopt_long")(argument_count, arguments, options, long_options, index);
  if (host_optarg) nuah_optarg = *host_optarg;
  if (host_optind) nuah_optind = *host_optind;
  return result;
}
#define FORWARD(ret, name, sig, args)               \
  ret name sig {                                    \
    static const auto fn = host<ret(*) sig>(#name); \
    return fn args;                                 \
  }
FORWARD(int, close, (int fd), (fd)) FORWARD(ssize_t, read, (int fd, void* d, size_t n), (fd, d, n)) NUAH_FORTIFY(ssize_t, __read_chk, (int fd, void* d, size_t n, size_t s), n > s, read(fd, d, n))
FORWARD(ssize_t, write, (int fd, const void* d, size_t n), (fd, d, n)) FORWARD(ssize_t, pread, (int fd, void* d, size_t n, off_t o), (fd, d, n, o)) FORWARD(ssize_t, pwrite, (int fd, const void* d, size_t n, off_t o), (fd, d, n, o))
FORWARD(ssize_t, writev, (int fd, const iovec* v, int c), (fd, v, c))
int open(const char* path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0
#ifdef O_TMPFILE
      || (flags & O_TMPFILE) == O_TMPFILE
#endif
  ) {
    va_list arguments;
    va_start(arguments, flags);
    mode = static_cast<mode_t>(va_arg(arguments, int));
    va_end(arguments);
  }
  static const auto fn = host<int (*)(const char*, int, mode_t)>("open");
  return fn(path, flags, mode);
}
FORWARD(int, __open_2, (const char* path, int flags), (path, flags)) FORWARD(int, access, (const char* path, int mode), (path, mode)) FORWARD(int, fchmod, (int fd, mode_t mode), (fd, mode)) FORWARD(int, fchown, (int fd, uid_t owner, gid_t group), (fd, owner, group))
int fcntl(int fd, int command, ...) {
  uintptr_t argument = 0;
  switch (command) {
    case F_GETFD: case F_GETFL: case F_GETOWN: break;
    default: {
      va_list arguments;
      va_start(arguments, command);
      argument = va_arg(arguments, uintptr_t);
      va_end(arguments);
      break;
    }
  }
  static const auto fn = host<int (*)(int, int, uintptr_t)>("fcntl");
  return fn(fd, command, argument);
}
FORWARD(int, fstat, (int fd, struct stat* v), (fd, v)) FORWARD(int, lstat, (const char* p, struct stat* v), (p, v)) FORWARD(int, stat, (const char* p, struct stat* v), (p, v))
FORWARD(int, fsync, (int fd), (fd)) FORWARD(int, ftruncate, (int fd, off_t l), (fd, l)) FORWARD(off_t, lseek, (int fd, off_t o, int origin), (fd, o, origin)) FORWARD(int, mkdir, (const char* p, mode_t m), (p, m))
void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
  static const auto fn = host<void* (*)(void*, size_t, int, int, int, off_t)>("mmap");
  void* result = fn(address, length, protection, flags, fd, offset);
  if (NUAH_LIKELY(result != MAP_FAILED) && (flags & MAP_ANONYMOUS) != 0 && length >= 2 * 1024 * 1024) {
#ifdef MADV_HUGEPAGE
    static const auto madv_fn = host<int (*)(void*, size_t, int)>("madvise");
    if (NUAH_LIKELY(madv_fn != nullptr)) { madv_fn(result, length, MADV_HUGEPAGE); }
#endif
  }
  return result;
}
int madvise(void* address, size_t length, int advice) {
  static const auto fn = host<int (*)(void*, size_t, int)>("madvise");
  if (NUAH_UNLIKELY(advice == 8)) { advice = MADV_DONTNEED; }
  return NUAH_LIKELY(fn != nullptr) ? fn(address, length, advice) : 0;
}
FORWARD(int, mprotect, (void* a, size_t l, int p), (a, l, p)) FORWARD(int, munmap, (void* a, size_t l), (a, l)) FORWARD(ssize_t, readlink, (const char* p, char* d, size_t l), (p, d, l))
NUAH_FORTIFY(ssize_t, __readlink_chk, (const char* p, char* d, size_t l, size_t s), l > s, readlink(p, d, l)) FORWARD(int, unlink, (const char* p), (p))
int __poll_chk(pollfd* descriptors, nfds_t count, int timeout, size_t descriptors_size) {
  if (count > descriptors_size / sizeof(*descriptors)) std::abort();
  static const auto fn = host<int (*)(pollfd*, nfds_t, int)>("poll");
  return fn(descriptors, count, timeout);
}
extern "C" size_t nuah_fread_chk(void* d, size_t sz, size_t c, std::FILE* st, size_t ds) __asm__("__fread_chk");
size_t nuah_fread_chk(void* d, size_t sz, size_t c, std::FILE* st, size_t ds) {
  if (sz && c > ds / sz) std::abort();
  return host<size_t (*)(void*, size_t, size_t, std::FILE*)>("fread")(d, sz, c, st);
}
size_t nuah_fread_chk_n(void* d, size_t sz, size_t c, std::FILE* st, size_t ds) { return nuah_fread_chk(d, sz, c, st, ds); }
asm(".symver nuah_fread_chk_n,__fread_chk@LIBC_N");
NUAH_FORTIFY(char*, __strncpy_chk, (char* d, const char* s, size_t c, size_t ds), c > ds, strncpy(d, s, c))
NUAH_FORTIFY(int, __vsnprintf_chk, (char* d, size_t sz, int, size_t ds, const char* fmt, va_list ap), sz > ds, host<int (*)(char*, size_t, const char*, va_list)>("vsnprintf")(d, sz, fmt, ap))
int __vsprintf_chk(char* d, int, size_t ds, const char* fmt, va_list ap) {
  const int res = host<int (*)(char*, size_t, const char*, va_list)>("vsnprintf")(d, ds, fmt, ap);
  if (res < 0 || static_cast<size_t>(res) >= ds) std::abort();
  return res;
}
void __assert(const char* f, int l, const char* e) {
  std::fprintf(stderr, "[bionic] assertion %s:%d: %s\n", f ? f : "?", l, e ? e : "?");
  std::abort();
}
[[noreturn]] void __stack_chk_fail() {
  std::fprintf(stderr, "[bionic] stack corruption detected\n");
  std::abort();
}
extern "C" size_t nuah_ctype_get_mb_cur_max() __asm__("__ctype_get_mb_cur_max");
size_t nuah_ctype_get_mb_cur_max() { return host<size_t (*)()>("__ctype_get_mb_cur_max")(); }
extern "C" cmsghdr* nuah_cmsg_nxthdr(msghdr* m, cmsghdr* c) __asm__("__cmsg_nxthdr");
cmsghdr* nuah_cmsg_nxthdr(msghdr* m, cmsghdr* c) { return host<cmsghdr* (*)(msghdr*, cmsghdr*)>("__cmsg_nxthdr")(m, c); }
void __cxa_finalize(void* dso) { host<void (*)(void*)>("__cxa_finalize")(dso); }
int __register_atfork(void (*p)(void), void (*pa)(void), void (*c)(void), void* d) { return host<int (*)(void (*)(), void (*)(), void (*)(), void*)>("__register_atfork")(p, pa, c, d); }
#define NUAH_TLS_LOG(name, fmt, ...) do { if (tls_trace_slot()) { std::fprintf(stderr, fmt, ##__VA_ARGS__); std::fprintf(stderr, "nuah bionic: " name " caller=%p\n", __builtin_return_address(0)); } } while (0)
int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
  if (android_key_namespace_enabled()) {
    pthread_key_t host_key = 0;
    const int result = nuah_host_pthread_key_create(&host_key, destructor);
    if (result != 0) return result;
    for (std::size_t index = 0; index < kAndroidKeySlots; ++index) {
      bool expected = false;
      if (!android_key_slots[index].used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
      android_key_slots[index].host_key.store(host_key, std::memory_order_release);
      *key = kAndroidKeyBase + static_cast<pthread_key_t>(index);
      NUAH_TLS_LOG("pthread_key_create", "nuah bionic: pthread_key_create android=%u host=%u\n", static_cast<unsigned>(*key), static_cast<unsigned>(host_key));
      return 0;
    }
    (void)nuah_host_pthread_key_delete(host_key);
    return EAGAIN;
  }
  const int result = nuah_host_pthread_key_create(key, destructor);
  NUAH_TLS_LOG("pthread_key_create", "nuah bionic: pthread_key_create key=%p value=%u result=%d\n", static_cast<void*>(key), result == 0 ? static_cast<unsigned>(*key) : 0u, result);
  return result;
}
int pthread_key_delete(pthread_key_t key) {
  const pthread_key_t host_key = host_key_for_android(key);
  const int result = nuah_host_pthread_key_delete(host_key);
  if (android_key_namespace_enabled() && key >= kAndroidKeyBase && key - kAndroidKeyBase < kAndroidKeySlots) {
    const auto index = static_cast<std::size_t>(key - kAndroidKeyBase);
    android_key_slots[index].used.store(false, std::memory_order_release);
  }
  return result;
}
void* pthread_getspecific(pthread_key_t key) {
  if (__builtin_expect(!android_key_namespace_enabled() && !tls_trace_slot(), 1)) { return nuah_host_pthread_getspecific(key); }
  const pthread_key_t host_key = host_key_for_android(key);
  const bool guard_host_key = pthread_tls_guard_enabled() && android_key_namespace_enabled() && host_key == 4;
  void* value = guard_host_key ? nullptr : nuah_host_pthread_getspecific(host_key);
  NUAH_TLS_LOG("pthread_getspecific", "nuah bionic: pthread_getspecific key=%u value=%p\n", static_cast<unsigned>(key), value);
  return value;
}
int nuah_pthread_setspecific_export(pthread_key_t key, const void* value) __asm__("pthread_setspecific");
int nuah_pthread_setspecific_export(pthread_key_t key, const void* value) {
  if (__builtin_expect(!android_key_namespace_enabled() && !tls_trace_slot(), 1)) { return nuah_host_pthread_setspecific(key, value); }
  using Function = int (*)(pthread_key_t, const void*);
  const Function function = &nuah_host_pthread_setspecific;
  const pthread_key_t host_key = host_key_for_android(key);
  const bool guard_host_key = pthread_tls_guard_enabled() && android_key_namespace_enabled() && host_key == 4;
  const uintptr_t value_bits = reinterpret_cast<uintptr_t>(value);
  const int result = guard_host_key ? 0 : function(host_key, reinterpret_cast<const void*>(value_bits));
  NUAH_TLS_LOG("pthread_setspecific", "nuah bionic: pthread_setspecific key=%u value=%p result=%d\n", static_cast<unsigned>(key), value, result);
  return result;
}
int pthread_once(pthread_once_t* object, void (*function)(void)) {
  const bool trace = sync_trace_slot();
  if (trace) { std::fprintf(stderr, "nuah bionic: pthread_once object=%p function=%p\n", static_cast<void*>(object), reinterpret_cast<void*>(function)); }
  auto* entry = once_for(object);
  const int result = entry ? nuah_host_pthread_once(&entry->native, function) : ENOMEM;
  if (trace) { std::fprintf(stderr, "nuah bionic: pthread_once object=%p native=%p result=%d\n", static_cast<void*>(object), entry ? static_cast<void*>(&entry->native) : nullptr, result); }
  return result;
}
int __cxa_atexit(void (*function)(void*), void* argument, void* dso) { return host<int (*)(void (*)(void*), void*, void*)>("__cxa_atexit")(function, argument, dso); }
int __cxa_thread_atexit_impl(void (*function)(void*), void* argument, void* dso) {
  // Android DSO handles are loader-owned; forwarding into glibc recurses at teardown.
  (void)function; (void)argument; (void)dso;
  return 0;
}
void __FD_CLR_chk(int, void*, size_t) {} int __FD_ISSET_chk(int, const void*, size_t) { return 0; } void __FD_SET_chk(int, void*, size_t) {}
void __assert2(const char* file, int line, const char* function, const char* expression) { std::fprintf(stderr, "[bionic] assertion %s:%d %s: %s\n", file ? file : "?", line, function ? function : "?", expression ? expression : "?"); }
int* __errno() { return &errno; }
char* __gnu_strerror_r(int error, char* buffer, size_t length) { if (buffer && length) std::snprintf(buffer, length, "%s", std::strerror(error)); return buffer; }
char* __strchr_chk(const char* text, int character, size_t) { return const_cast<char*>(std::strchr(text, character)); }
size_t __strlen_chk(const char* text, size_t) { return std::strlen(text); }
char* __strncpy_chk2(char* destination, const char* source, size_t count, size_t, size_t) { return std::strncpy(destination, source, count); }
int __system_property_get(const char* key, char* value) {
  static const struct { const char* key; const char* value; } kProps[] = {{"ro.product.cpu.abi", "x86_64"}, {"ro.build.version.sdk", NUAH_STRINGIFY(NUAH_ANDROID_API_LEVEL)}, {"ro.build.version.release", "10"}};
  const char* result = "";
  for (const auto& p : kProps) if (key && std::strcmp(key, p.key) == 0) { result = p.value; break; }
  if (value) std::strcpy(value, result);
  if (diagnostics_callbacks.record_property) { diagnostics_callbacks.record_property(key, result); }
  return static_cast<int>(std::strlen(result));
}
void android_set_abort_message(const char* message) {
  // Bionic clients use this to explain an imminent abort.  Retaining it as a
  // no-op turns a recoverable compatibility diagnosis into an opaque SIGABRT.
  constexpr size_t kMaximumMessage = 1024;
  const size_t length = message ? ::strnlen(message, kMaximumMessage) : 0;
  std::fprintf(stderr, "nuah bootstrap: Android abort message: %.*s%s\n", static_cast<int>(length), message ? message : "(null)", length == kMaximumMessage ? "…" : "");
  std::fflush(stderr);
  if (diagnostics_callbacks.record_abort_message) { diagnostics_callbacks.record_abort_message(message); }
}
size_t __fwrite_chk(const void* data, size_t size, size_t count, std::FILE* stream, size_t) { return std::fwrite(data, size, count, stream); }
ssize_t __write_chk(int fd, const void* data, size_t count, size_t) { return ::write(fd, data, count); }
ssize_t __sendto_chk(int fd, const void* data, size_t count, size_t, int flags, const sockaddr* address, socklen_t length) { return ::sendto(fd, data, count, flags, address, length); }
size_t nuah_fwrite_chk_n(const void* data, size_t size, size_t count, std::FILE* stream, size_t data_size) { return __fwrite_chk(data, size, count, stream, data_size); }
ssize_t nuah_write_chk_n(int fd, const void* data, size_t count, size_t data_size) { return __write_chk(fd, data, count, data_size); }
ssize_t nuah_sendto_chk_o(int fd, const void* data, size_t count, size_t data_size, int flags, const sockaddr* address, socklen_t length) { return __sendto_chk(fd, data, count, data_size, flags, address, length); }
asm(".symver nuah_fwrite_chk_n,__fwrite_chk@LIBC_N"); asm(".symver nuah_write_chk_n,__write_chk@LIBC_N"); asm(".symver nuah_sendto_chk_o,__sendto_chk@LIBC_O");
// Android's pthread_mutex_t/pthread_cond_t layout is ABI-incompatible with
// glibc. Keep host synchronization objects out-of-line and use the Android
// object address purely as Nuah's stable key.
#define NUAH_SYNC_TRACE(...)                      \
  do {                                            \
    if (sync_trace_slot()) std::fprintf(stderr, __VA_ARGS__); \
  } while (0)
#define NUAH_ATTR_OP(object, expr)                \
  do {                                            \
    auto* entry = attr_for(object);               \
    return entry ? (expr) : EINVAL;                \
  } while (0)
#define NUAH_RWLOCK_OP(object, expr)              \
  do {                                            \
    auto* e = rwlock_for(object);                 \
    return e ? (expr) : EINVAL;                    \
  } while (0)
#define NUAH_ENGINE_BEGIN(started, caller) const bool nuah_engine_trace = engine_trace_enabled(); const uint64_t started = nuah_engine_trace ? monotonic_ns() : 0; const void* caller = nuah_engine_trace ? __builtin_return_address(0) : nullptr
#define NUAH_ENGINE_END(kind, started, caller) do { if (nuah_engine_trace) record_sync_wait(kind, started, caller); } while (0)
#define NUAH_MUTEX_PLAIN_OP(name, host_fn, dflt) int name(pthread_mutex_t* o) { auto* e = mutex_for(o); const int r = e ? host_fn(&e->native) : dflt; NUAH_SYNC_TRACE("nuah bionic: " #name " android=%p native=%p result=%d\n", static_cast<void*>(o), e ? static_cast<void*>(&e->native) : nullptr, r); return r; }
#define NUAH_COND_WAIT_OP(name, host_fn, kind) int name(pthread_cond_t* cnd, pthread_mutex_t* mtx) { NUAH_ENGINE_BEGIN(started_ns, caller); auto* c = cond_for(cnd); auto* m = mutex_for(mtx); const int r = c && m ? host_fn(&c->native, &m->native) : ENOMEM; NUAH_ENGINE_END(kind, started_ns, caller); return r; }
#define NUAH_COND_WAIT_T_OP(name, host_fn, kind) int name(pthread_cond_t* cnd, pthread_mutex_t* mtx, const timespec* tmo) { NUAH_ENGINE_BEGIN(started_ns, caller); auto* c = cond_for(cnd); auto* m = mutex_for(mtx); const int r = c && m ? host_fn(&c->native, &m->native, tmo) : ENOMEM; NUAH_ENGINE_END(kind, started_ns, caller); return r; }
}  // extern "C" (templates require C++ linkage)
template <typename Entry, std::size_t Count, typename Fn>
int destroy_entry(Entry (&table)[Count], void* object, Fn destroy) {
  auto* e = begin_destroy(table, object);
  if (!e) return EINVAL;
  const int r = destroy(e->native);
  if (r == 0) clear_entry(*e, object); else publish_entry(*e, true);
  return r;
}
extern "C" {
int pthread_mutex_init(pthread_mutex_t* object, const pthread_mutexattr_t* attributes) {
  auto* entry = mutex_for(object, attributes);
  NUAH_SYNC_TRACE("nuah bionic: pthread_mutex_init android=%p native=%p result=%d\n", static_cast<void*>(object), entry ? static_cast<void*>(&entry->native) : nullptr, entry ? 0 : ENOMEM);
  return entry ? 0 : ENOMEM;
}
int pthread_mutex_destroy(pthread_mutex_t* object) { NUAH_SYNC_TRACE("nuah bionic: pthread_mutex_destroy android=%p\n", static_cast<void*>(object)); return 0; }
int pthread_mutex_lock(pthread_mutex_t* object) {
  NUAH_ENGINE_BEGIN(started_ns, caller); auto* entry = mutex_for(object);
  const int result = entry ? nuah_host_mutex_lock(&entry->native) : ENOMEM;
  NUAH_ENGINE_END("mutex_lock", started_ns, caller);
  NUAH_SYNC_TRACE("nuah bionic: pthread_mutex_lock android=%p native=%p result=%d\n", static_cast<void*>(object), entry ? static_cast<void*>(&entry->native) : nullptr, result);
  return result;
}
NUAH_MUTEX_PLAIN_OP(pthread_mutex_trylock, nuah_host_mutex_trylock, ENOMEM)
NUAH_MUTEX_PLAIN_OP(pthread_mutex_unlock, nuah_host_mutex_unlock, EINVAL)
int pthread_mutexattr_init(pthread_mutexattr_t* object) { return mutex_attr_for(object) ? 0 : ENOMEM; }
int pthread_mutexattr_destroy(pthread_mutexattr_t* object) {
  lock_table();
  if (auto* found = find_mutex_attr_locked(object)) { found->android = nullptr; found->type = PTHREAD_MUTEX_NORMAL; unlock_table(); return 0; }
  unlock_table(); return EINVAL;
}
int pthread_mutexattr_settype(pthread_mutexattr_t* object, int type) {
  if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE && type != PTHREAD_MUTEX_ERRORCHECK) { return EINVAL; }
  auto* entry = mutex_attr_for(object);
  if (!entry) return ENOMEM;
  entry->type = type; return 0;
}
int pthread_cond_init(pthread_cond_t* object, const pthread_condattr_t*) { return cond_for(object) ? 0 : ENOMEM; } int pthread_cond_destroy(pthread_cond_t*) { return 0; }
int pthread_cond_signal(pthread_cond_t* o) { auto* e = cond_for(o); return e ? nuah_host_cond_signal(&e->native) : ENOMEM; } int pthread_cond_broadcast(pthread_cond_t* o) { auto* e = cond_for(o); return e ? nuah_host_cond_broadcast(&e->native) : ENOMEM; }
NUAH_COND_WAIT_OP(pthread_cond_wait, nuah_host_cond_wait, "cond_wait")
NUAH_COND_WAIT_T_OP(pthread_cond_timedwait, nuah_host_cond_timedwait, "cond_timedwait")
int pthread_condattr_init(pthread_condattr_t*) { return 0; } int pthread_condattr_destroy(pthread_condattr_t*) { return 0; } int pthread_condattr_setclock(pthread_condattr_t*, clockid_t) { return 0; }
int pthread_attr_init(pthread_attr_t* o) { return attr_for(o) ? 0 : ENOMEM; }
int pthread_attr_destroy(pthread_attr_t* o) { auto fn = host<int (*)(pthread_attr_t*)>("pthread_attr_destroy"); return destroy_entry(attributes, o, [&](pthread_attr_t& native) { return fn ? fn(&native) : EINVAL; }); }
int pthread_attr_getstack(const pthread_attr_t* o, void** a, size_t* s) {
  auto* e = attr_for(const_cast<pthread_attr_t*>(o)); int r = e ? host<int (*)(const pthread_attr_t*, void**, size_t*)>("pthread_attr_getstack")(&e->native, a, s) : EINVAL;
  if (sync_trace_slot()) std::fprintf(stderr, "nuah bionic: pthread_attr_getstack android=%p base=%p size=%zu result=%d\n", static_cast<const void*>(o), *a, *s, r);
  return r;
}
int pthread_attr_setdetachstate(pthread_attr_t* o, int s) { NUAH_ATTR_OP(o, host<int (*)(pthread_attr_t*, int)>("pthread_attr_setdetachstate")(&entry->native, s)); } int pthread_attr_setschedparam(pthread_attr_t* o, const sched_param* p) { NUAH_ATTR_OP(o, host<int (*)(pthread_attr_t*, const sched_param*)>("pthread_attr_setschedparam")(&entry->native, p)); }
int pthread_attr_setstacksize(pthread_attr_t* o, size_t s) {
  auto* e = attr_for(o); int r = e ? host<int (*)(pthread_attr_t*, size_t)>("pthread_attr_setstacksize")(&e->native, s) : EINVAL;
  if (bootstrap_trace_enabled()) std::fprintf(stderr, "nuah bionic: pthread_attr_setstacksize android=%p size=%zu result=%d\n", static_cast<void*>(o), s, r);
  return r;
}
int pthread_create(pthread_t* thread, const pthread_attr_t* object, void* (*start)(void*), void* argument) {
  auto* entry = object ? attr_for(const_cast<pthread_attr_t*>(object)) : nullptr;
  constexpr size_t kMinimumRobloxStack = 256u * 1024u * 1024u;
  pthread_attr_t fallback{};
  const pthread_attr_t* host_attributes = entry ? &entry->native : nullptr;
  bool destroy_fallback = false;
  if (!host_attributes) {
    if (host<int (*)(pthread_attr_t*)>("pthread_attr_init")(&fallback) != 0 || host<int (*)(pthread_attr_t*, size_t)>("pthread_attr_setstacksize")(&fallback, kMinimumRobloxStack) != 0) {
      return EAGAIN;
    }
    host_attributes = &fallback;
    destroy_fallback = true;
  } else {
    size_t stack_size = 0;
    if (host<int (*)(const pthread_attr_t*, size_t*)>("pthread_attr_getstacksize")(host_attributes, &stack_size) == 0 && stack_size < kMinimumRobloxStack) {
      (void)host<int (*)(pthread_attr_t*, size_t)>("pthread_attr_setstacksize")(const_cast<pthread_attr_t*>(host_attributes), kMinimumRobloxStack);
    }
  }
  if (bootstrap_trace_enabled()) {
    size_t stack_size = 0;
    (void)host<int (*)(const pthread_attr_t*, size_t*)>("pthread_attr_getstacksize")(host_attributes, &stack_size);
    std::fprintf(stderr,
                 "nuah bionic: pthread_create android_attr=%p "
                 "host_stack_size=%zu start=%p\n",
                 static_cast<const void*>(object), stack_size, reinterpret_cast<void*>(start));
  }
  const int result = host<int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*)>("pthread_create")(thread, host_attributes, start, argument);
  if (sync_trace_slot() && result == 0) {
    pthread_attr_t observed{};
    void* stack_base = nullptr;
    size_t observed_size = 0;
    const int observed_result = host<int (*)(pthread_t, pthread_attr_t*)>("pthread_getattr_np")(*thread, &observed);
    if (observed_result == 0) {
      (void)host<int (*)(const pthread_attr_t*, void**, size_t*)>("pthread_attr_getstack")(&observed, &stack_base, &observed_size);
      (void)host<int (*)(pthread_attr_t*)>("pthread_attr_destroy")(&observed);
    }
    std::fprintf(stderr,
                 "nuah bionic: pthread_create result=0 thread=%p "
                 "actual_stack=%p actual_size=%zu inspect=%d\n",
                 reinterpret_cast<void*>(*thread), stack_base, observed_size, observed_result);
  }
  if (destroy_fallback) { (void)host<int (*)(pthread_attr_t*)>("pthread_attr_destroy")(&fallback); }
  return result;
}
int pthread_getattr_np(pthread_t thread, pthread_attr_t* object) {
  auto* entry = attr_for(object);
  return entry ? host<int (*)(pthread_t, pthread_attr_t*)>("pthread_getattr_np")(thread, &entry->native) : EINVAL;
}
FORWARD(int, pthread_detach, (pthread_t t), (t)) FORWARD(int, pthread_equal, (pthread_t a, pthread_t b), (a, b))
[[noreturn]] void pthread_exit(void* v) { host<void (*)(void*)>("pthread_exit")(v); __builtin_unreachable(); }
FORWARD(int, pthread_getschedparam, (pthread_t t, int* p, sched_param* s), (t, p, s)) FORWARD(int, pthread_join, (pthread_t t, void** v), (t, v)) FORWARD(int, pthread_kill, (pthread_t t, int s), (t, s))
FORWARD(pthread_t, pthread_self, (), ()) FORWARD(int, pthread_setname_np, (pthread_t t, const char* n), (t, n)) FORWARD(int, pthread_setschedparam, (pthread_t t, int p, const sched_param* s), (t, p, s)) FORWARD(int, pthread_sigmask, (int o, const sigset_t* s, sigset_t* old_s), (o, s, old_s))
int pthread_rwlock_init(pthread_rwlock_t* o, const pthread_rwlockattr_t*) { return rwlock_for(o) ? 0 : ENOMEM; }
int pthread_rwlock_destroy(pthread_rwlock_t* o) { return destroy_entry(rwlocks, o, [](pthread_rwlock_t& native) { return nuah_host_rwlock_destroy(&native); }); }
int pthread_rwlock_rdlock(pthread_rwlock_t* o) { NUAH_RWLOCK_OP(o, nuah_host_rwlock_rdlock(&e->native)); } int pthread_rwlock_wrlock(pthread_rwlock_t* o) { NUAH_RWLOCK_OP(o, nuah_host_rwlock_wrlock(&e->native)); } int pthread_rwlock_unlock(pthread_rwlock_t* o) { NUAH_RWLOCK_OP(o, nuah_host_rwlock_unlock(&e->native)); }
#define NUAH_SEM_OP(object, expr)             \
  do {                                        \
    auto* e = sem_for(object);                \
    if (!e) { errno = EINVAL; return -1; }    \
    return expr;                              \
  } while (0)
int sem_wait(sem_t* o) { NUAH_SEM_OP(o, host<int (*)(sem_t*)>("sem_wait")(&e->native)); } int sem_post(sem_t* o) { NUAH_SEM_OP(o, host<int (*)(sem_t*)>("sem_post")(&e->native)); }
int sem_init(sem_t* o, int sh, unsigned int val) {
  bool init = false; auto* e = find_or_reserve(semaphores, o, init);
  if (!e) { errno = ENOMEM; return -1; }
  if (!init) return 0;
  auto fn = host<int (*)(sem_t*, int, unsigned int)>("sem_init"); int r = fn ? fn(&e->native, sh, val) : -1;
  publish_entry(*e, r == 0);
  if (r != 0) clear_entry(*e, o);
  return r;
}
int sem_destroy(sem_t* o) {
  auto* e = begin_destroy(semaphores, o); if (!e) { errno = EINVAL; return -1; }
  auto fn = host<int (*)(sem_t*)>("sem_destroy"); int r = fn ? fn(&e->native) : -1;
  if (r == 0) clear_entry(*e, o); else publish_entry(*e, true);
  return r;
}
}  // extern "C"
