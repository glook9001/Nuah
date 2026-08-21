#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

constexpr int kPollWake = -1;
constexpr int kPollCallback = -2;
constexpr int kPollTimeout = -3;
constexpr int kPollError = -4;
constexpr std::size_t kMaxRegistrations = 32;

bool trace_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("NUAH_BOOTSTRAP_TRACE");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

using Callback = int (*)(int, int, void*);

struct Registration {
  int fd = -1;
  int ident = 0;
  int events = 0;
  Callback callback = nullptr;
  void* data = nullptr;
};

struct Looper {
  Looper()
      : epoll_fd(::epoll_create1(EPOLL_CLOEXEC)),
        wake_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (epoll_fd < 0 || wake_fd < 0) {
      if (epoll_fd >= 0) ::close(epoll_fd);
      if (wake_fd >= 0) ::close(wake_fd);
      epoll_fd = -1;
      wake_fd = -1;
      return;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = wake_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &event) != 0) {
      ::close(epoll_fd);
      ::close(wake_fd);
      epoll_fd = -1;
      wake_fd = -1;
    }
  }

  ~Looper() {
    if (epoll_fd >= 0) ::close(epoll_fd);
    if (wake_fd >= 0) ::close(wake_fd);
  }

  Looper(const Looper&) = delete;
  Looper& operator=(const Looper&) = delete;

  int epoll_fd = -1;
  int wake_fd = -1;
  std::atomic<int> refs{1};
  std::atomic<bool> polling{false};
  unsigned int polls = 0;
  unsigned int empty_polls = 0;
  std::mutex mutex;
  std::array<Registration, kMaxRegistrations> registrations{};
  std::size_t registration_count = 0;
};

thread_local Looper thread_looper;

uint32_t epoll_events_from_android(int events) {
  uint32_t result = 0;
  if ((events & (1 << 0)) != 0) result |= EPOLLIN;
  if ((events & (1 << 1)) != 0) result |= EPOLLOUT;
  /* Error/hangup are reported by epoll even when they are not requested. */
  return result;
}

int android_events_from_epoll(uint32_t events) {
  int result = 0;
  if ((events & EPOLLIN) != 0) result |= 1 << 0;
  if ((events & EPOLLOUT) != 0) result |= 1 << 1;
  if ((events & EPOLLERR) != 0) result |= 1 << 2;
  if ((events & EPOLLHUP) != 0) result |= 1 << 3;
  if ((events & EPOLLPRI) != 0) result |= 1 << 4;
  return result;
}

Registration find_registration(Looper* looper, int fd, bool* found) {
  std::scoped_lock lock(looper->mutex);
  for (std::size_t i = 0; i < looper->registration_count; ++i) {
    if (looper->registrations[i].fd == fd) {
      if (found) *found = true;
      return looper->registrations[i];
    }
  }
  if (found) *found = false;
  return {};
}

}  // namespace

extern "C" {

void* ALooper_forThread() { return &thread_looper; }
void* ALooper_prepare(int) { return ALooper_forThread(); }

void ALooper_acquire(void* opaque) {
  if (opaque) static_cast<Looper*>(opaque)->refs.fetch_add(1, std::memory_order_relaxed);
}

void ALooper_release(void* opaque) {
  /* Loopers are thread-local.  Keep the object alive until thread teardown;
   * freeing it from an arbitrary release would make later Android callbacks
   * use-after-free. */
  if (opaque) static_cast<Looper*>(opaque)->refs.fetch_sub(1, std::memory_order_relaxed);
}

int ALooper_addFd(void* opaque, int fd, int ident, int events,
                  int (*callback)(int, int, void*), void* data) {
  auto* looper = static_cast<Looper*>(opaque);
  if (!looper || looper->epoll_fd < 0 || fd < 0 || (!callback && ident < 0)) {
    errno = EINVAL;
    return 0;
  }

  epoll_event event{};
  event.events = epoll_events_from_android(events);
  event.data.fd = fd;
  std::scoped_lock lock(looper->mutex);
  std::size_t index = looper->registration_count;
  for (std::size_t i = 0; i < looper->registration_count; ++i) {
    if (looper->registrations[i].fd == fd) {
      index = i;
      break;
    }
  }
  const bool update = index < looper->registration_count;
  if (!update && looper->registration_count == kMaxRegistrations) {
    errno = ENOSPC;
    return 0;
  }
  const int operation = update ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (::epoll_ctl(looper->epoll_fd, operation, fd, &event) != 0) return 0;
  looper->registrations[index] = {fd, ident, events, callback, data};
  if (!update) ++looper->registration_count;
  if (trace_enabled()) {
    std::fprintf(stderr,
                 "nuah looper-fast: add thread=%ld looper=%p fd=%d ident=%d\n",
                 static_cast<long>(::gettid()), opaque, fd, ident);
  }
  return 1;
}

int ALooper_removeFd(void* opaque, int fd) {
  auto* looper = static_cast<Looper*>(opaque);
  if (!looper || looper->epoll_fd < 0) return 0;
  std::scoped_lock lock(looper->mutex);
  for (std::size_t i = 0; i < looper->registration_count; ++i) {
    if (looper->registrations[i].fd != fd) continue;
    (void)::epoll_ctl(looper->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    looper->registrations[i] = looper->registrations[--looper->registration_count];
    return 1;
  }
  return 0;
}

void ALooper_wake(void* opaque) {
  auto* looper = static_cast<Looper*>(opaque);
  if (!looper || looper->wake_fd < 0) return;
  const uint64_t value = 1;
  const ssize_t written = ::write(looper->wake_fd, &value, sizeof(value));
  (void)written;
}

bool ALooper_isPolling(void* opaque) {
  return opaque && static_cast<Looper*>(opaque)->polling.load(std::memory_order_relaxed);
}

int ALooper_pollOnce(int timeout_ms, int* out_fd, int* out_events,
                     void** out_data) {
  auto* looper = static_cast<Looper*>(ALooper_forThread());
  if (!looper || looper->epoll_fd < 0) return kPollError;

  /* Keep Android's non-blocking probe contract, but stop an empty probe loop
   * from consuming a host core.  This is only a 1 ms yield after eight empty
   * probes and is reset as soon as an fd or wake event arrives. */
  int effective_timeout = timeout_ms;
  if (timeout_ms == 0 && looper->empty_polls >= 8) effective_timeout = 1;

  std::array<epoll_event, kMaxRegistrations + 1> events{};
  looper->polling.store(true, std::memory_order_relaxed);
  const int ready = ::epoll_wait(looper->epoll_fd, events.data(),
                                  static_cast<int>(events.size()),
                                  effective_timeout);
  looper->polling.store(false, std::memory_order_relaxed);
  if (trace_enabled() && looper->polls++ < 12) {
    std::fprintf(stderr,
                 "nuah looper-fast: poll thread=%ld sources=%zu timeout=%d ready=%d\n",
                 static_cast<long>(::gettid()), looper->registration_count,
                 timeout_ms, ready);
  }
  if (ready == 0) {
    if (looper->empty_polls != UINT_MAX) ++looper->empty_polls;
    return kPollTimeout;
  }
  if (ready < 0) return errno == EINTR ? kPollTimeout : kPollError;
  looper->empty_polls = 0;

  for (int i = 0; i < ready; ++i) {
    const int fd = events[static_cast<std::size_t>(i)].data.fd;
    if (fd == looper->wake_fd) {
      uint64_t value = 0;
      while (::read(looper->wake_fd, &value, sizeof(value)) == sizeof(value)) {}
      return kPollWake;
    }
    bool found = false;
    const Registration registration = find_registration(looper, fd, &found);
    if (!found) continue;  // removed while epoll_wait was in progress
    const int android_events =
        android_events_from_epoll(events[static_cast<std::size_t>(i)].events);
    if (registration.callback) {
      const int keep = registration.callback(registration.fd, android_events,
                                             registration.data);
      if (!keep) (void)ALooper_removeFd(looper, registration.fd);
      return kPollCallback;
    }
    if (out_fd) *out_fd = registration.fd;
    if (out_events) *out_events = android_events;
    if (out_data) *out_data = registration.data;
    return registration.ident;
  }
  return kPollTimeout;
}

int ALooper_pollAll(int timeout_ms, int* out_fd, int* out_events,
                    void** out_data) {
  return ALooper_pollOnce(timeout_ms, out_fd, out_events, out_data);
}

}  // extern "C"
