#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
namespace {
constexpr int kPollWake = -1, kPollCallback = -2, kPollTimeout = -3, kPollError = -4; constexpr std::size_t kMaxRegistrations = 32;
// getenv flag, zero-alloc hot path: shared env-gate helper.
#define NUAH_ENV_FLAG(fn, name) bool fn() { static const bool e = [] { const char* v = std::getenv(name); return v && *v && std::strcmp(v, "0") != 0; }(); return e; }
NUAH_ENV_FLAG(trace_enabled, "NUAH_BOOTSTRAP_TRACE")
using Callback = int (*)(int, int, void*);
struct Registration { int fd = -1; int ident = 0; Callback callback = nullptr; void* data = nullptr; };
struct Looper {
  Looper() : epoll_fd(::epoll_create1(EPOLL_CLOEXEC)), wake_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) { epoll_event event{}; event.events = EPOLLIN; event.data.fd = wake_fd;
    if (epoll_fd < 0 || wake_fd < 0 || ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &event) != 0) { if (epoll_fd >= 0) ::close(epoll_fd); if (wake_fd >= 0) ::close(wake_fd); epoll_fd = -1; wake_fd = -1; } }
  ~Looper() { if (epoll_fd >= 0) ::close(epoll_fd); if (wake_fd >= 0) ::close(wake_fd); }
  Looper(const Looper&) = delete; Looper& operator=(const Looper&) = delete;
  int epoll_fd = -1; int wake_fd = -1; std::atomic<int> refs{1}; std::atomic<bool> polling{false}; unsigned int polls = 0; std::mutex mutex;
  std::array<Registration, kMaxRegistrations> registrations{}; std::size_t registration_count = 0; };
thread_local Looper thread_looper;
uint32_t epoll_events_from_android(int events) { return ((events & 1) ? EPOLLIN : 0U) | ((events & 2) ? EPOLLOUT : 0U); /* Error/hangup reported even when not requested. */ }
int android_events_from_epoll(uint32_t events) { return ((events & EPOLLIN) ? 1 : 0) | ((events & EPOLLOUT) ? 2 : 0) | ((events & EPOLLERR) ? 4 : 0) | ((events & EPOLLHUP) ? 8 : 0) | ((events & EPOLLPRI) ? 16 : 0); }
static long reg_index(Looper* looper, int fd) { for (std::size_t i = 0; i < looper->registration_count; ++i) if (looper->registrations[i].fd == fd) return static_cast<long>(i); return -1; }
// Zero-alloc lookup: copy under lock, no heap.
Registration find_registration(Looper* looper, int fd, bool* found) { std::scoped_lock lock(looper->mutex); const auto index = reg_index(looper, fd); if (found) *found = index >= 0;
  return index >= 0 ? looper->registrations[static_cast<std::size_t>(index)] : Registration{}; }
}  // namespace
extern "C" {
void* ALooper_forThread() { return &thread_looper; } void* ALooper_prepare(int) { return ALooper_forThread(); } void ALooper_acquire(void* opaque) { if (opaque) static_cast<Looper*>(opaque)->refs.fetch_add(1, std::memory_order_relaxed); }
void ALooper_release(void* opaque) { /* Thread-local: never free on release or later callbacks use-after-free. */ if (opaque) static_cast<Looper*>(opaque)->refs.fetch_sub(1, std::memory_order_relaxed); }
int ALooper_addFd(void* opaque, int fd, int ident, int events, int (*callback)(int, int, void*), void* data) { auto* looper = static_cast<Looper*>(opaque);
  if (!looper || looper->epoll_fd < 0 || fd < 0 || (!callback && ident < 0)) { errno = EINVAL; return 0; }
  epoll_event event{}; event.events = epoll_events_from_android(events); event.data.fd = fd; std::scoped_lock lock(looper->mutex);
  const long found_index = reg_index(looper, fd); const bool update = found_index >= 0;
  std::size_t index = update ? static_cast<std::size_t>(found_index) : looper->registration_count;
  if (!update && looper->registration_count == kMaxRegistrations) { errno = ENOSPC; return 0; }
  const int operation = update ? EPOLL_CTL_MOD : EPOLL_CTL_ADD; if (::epoll_ctl(looper->epoll_fd, operation, fd, &event) != 0) return 0;
  looper->registrations[index] = {fd, ident, callback, data}; if (!update) ++looper->registration_count;
  if (trace_enabled()) { std::fprintf(stderr, "nuah looper-fast: add thread=%ld looper=%p fd=%d ident=%d\n", static_cast<long>(::gettid()), opaque, fd, ident); } return 1; }
int ALooper_removeFd(void* opaque, int fd) { auto* looper = static_cast<Looper*>(opaque); if (!looper || looper->epoll_fd < 0) return 0;
  std::scoped_lock lock(looper->mutex); const long found_index = reg_index(looper, fd); if (found_index < 0) return 0;
  (void)::epoll_ctl(looper->epoll_fd, EPOLL_CTL_DEL, fd, nullptr); looper->registrations[static_cast<std::size_t>(found_index)] = looper->registrations[--looper->registration_count]; return 1; }
void ALooper_wake(void* opaque) { auto* looper = static_cast<Looper*>(opaque); if (!looper || looper->wake_fd < 0) return; const uint64_t value = 1; const ssize_t written = ::write(looper->wake_fd, &value, sizeof(value)); (void)written; }
bool ALooper_isPolling(void* opaque) { return opaque && static_cast<Looper*>(opaque)->polling.load(std::memory_order_relaxed); }
int ALooper_pollOnce(int timeout_ms, int* out_fd, int* out_events, void** out_data) { auto* looper = static_cast<Looper*>(ALooper_forThread());
  if (!looper || looper->epoll_fd < 0) return kPollError;
  int effective_timeout = timeout_ms; std::array<epoll_event, kMaxRegistrations + 1> events{};
  looper->polling.store(true, std::memory_order_relaxed); const int ready = ::epoll_wait(looper->epoll_fd, events.data(), static_cast<int>(events.size()), effective_timeout); looper->polling.store(false, std::memory_order_relaxed);
  if (trace_enabled() && looper->polls++ < 12) { std::fprintf(stderr, "nuah looper-fast: poll thread=%ld sources=%zu timeout=%d ready=%d\n", static_cast<long>(::gettid()), looper->registration_count, timeout_ms, ready); }
  if (ready == 0) return kPollTimeout;
  if (ready < 0) return errno == EINTR ? kPollTimeout : kPollError;
  bool had_callback = false; bool had_wake = false;
  for (int i = 0; i < ready; ++i) { const int fd = events[static_cast<std::size_t>(i)].data.fd;
    if (fd == looper->wake_fd) { uint64_t value = 0; (void)::read(looper->wake_fd, &value, sizeof(value)); had_wake = true; continue; }
    bool found = false; const Registration registration = find_registration(looper, fd, &found); if (!found) continue;  // removed while epoll_wait was in progress
    const int android_events = android_events_from_epoll(events[static_cast<std::size_t>(i)].events);
    if (registration.callback) { const int keep = registration.callback(registration.fd, android_events, registration.data); if (!keep) (void)ALooper_removeFd(looper, registration.fd); had_callback = true; }
    else { if (out_fd) *out_fd = registration.fd; if (out_events) *out_events = android_events; if (out_data) *out_data = registration.data; return registration.ident; } }
  if (had_callback) return kPollCallback;
  if (had_wake) return kPollWake;
  return kPollTimeout; }
int ALooper_pollAll(int timeout_ms, int* out_fd, int* out_events, void** out_data) { int result = ALooper_pollOnce(timeout_ms, out_fd, out_events, out_data);
  while (result == kPollCallback || result == kPollWake) result = ALooper_pollOnce(0, out_fd, out_events, out_data);
  return result; }
}  // extern "C"
