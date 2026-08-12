#include <array>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <semaphore.h>
#include <thread>
#include <vector>

#ifndef NUAH_TEST_BIONIC_PATH
#error "NUAH_TEST_BIONIC_PATH is required"
#endif

namespace {
template <typename T>
T symbol(void* library, const char* name) {
  return reinterpret_cast<T>(::dlsym(library, name));
}

struct AndroidObject {
  alignas(16) std::array<unsigned char, 64> bytes{};
};
}  // namespace

int main() {
  void* library = ::dlopen(NUAH_TEST_BIONIC_PATH, RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    std::fprintf(stderr, "bionic sync stress: %s\n", ::dlerror());
    return 1;
  }

  using MutexInit = int (*)(void*, const void*);
  using MutexLock = int (*)(void*);
  using MutexUnlock = int (*)(void*);
  using CondInit = int (*)(void*, const void*);
  using AttrInit = int (*)(void*);
  using AttrSetType = int (*)(void*, int);
  using AttrDestroy = int (*)(void*);
  using RwlockInit = int (*)(void*, const void*);
  using RwlockLock = int (*)(void*);
  using RwlockUnlock = int (*)(void*);
  using SemInit = int (*)(void*, int, unsigned int);
  using SemPost = int (*)(void*);
  using SemWait = int (*)(void*);

  const auto mutex_init = symbol<MutexInit>(library, "pthread_mutex_init");
  const auto mutex_lock = symbol<MutexLock>(library, "pthread_mutex_lock");
  const auto mutex_unlock = symbol<MutexUnlock>(library, "pthread_mutex_unlock");
  const auto cond_init = symbol<CondInit>(library, "pthread_cond_init");
  const auto attr_init = symbol<AttrInit>(library, "pthread_mutexattr_init");
  const auto attr_set_type =
      symbol<AttrSetType>(library, "pthread_mutexattr_settype");
  const auto attr_destroy =
      symbol<AttrDestroy>(library, "pthread_mutexattr_destroy");
  const auto rwlock_init = symbol<RwlockInit>(library, "pthread_rwlock_init");
  const auto rwlock_rdlock = symbol<RwlockLock>(library, "pthread_rwlock_rdlock");
  const auto rwlock_unlock = symbol<RwlockUnlock>(library, "pthread_rwlock_unlock");
  const auto sem_init = symbol<SemInit>(library, "sem_init");
  const auto sem_post = symbol<SemPost>(library, "sem_post");
  const auto sem_wait = symbol<SemWait>(library, "sem_wait");
  if (!mutex_init || !mutex_lock || !mutex_unlock || !cond_init || !attr_init ||
      !attr_set_type || !attr_destroy || !rwlock_init || !rwlock_rdlock ||
      !rwlock_unlock || !sem_init || !sem_post || !sem_wait) {
    std::fprintf(stderr, "bionic sync stress: required symbols missing\n");
    return 1;
  }

  constexpr unsigned kThreads = 24;
  constexpr unsigned kObjectsPerThread = 16;
  std::vector<AndroidObject> mutexes(kThreads * kObjectsPerThread);
  std::vector<AndroidObject> conditions(kThreads * kObjectsPerThread);
  std::vector<AndroidObject> attributes(kThreads * kObjectsPerThread);
  std::vector<AndroidObject> rwlocks(kThreads * kObjectsPerThread);
  std::vector<AndroidObject> semaphores(kThreads * kObjectsPerThread);
  std::atomic<bool> go{false};
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (unsigned thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&, thread] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (unsigned index = 0; index < kObjectsPerThread; ++index) {
        const auto object = thread * kObjectsPerThread + index;
        auto* mutex = mutexes[object].bytes.data();
        auto* condition = conditions[object].bytes.data();
        auto* attribute = attributes[object].bytes.data();
        auto* rwlock = rwlocks[object].bytes.data();
        auto* semaphore = semaphores[object].bytes.data();
        if (attr_init(attribute) != 0 || attr_set_type(attribute, 1) != 0 ||
            attr_destroy(attribute) != 0 || mutex_init(mutex, nullptr) != 0 ||
            mutex_lock(mutex) != 0 || mutex_unlock(mutex) != 0 ||
            cond_init(condition, nullptr) != 0 ||
            rwlock_init(rwlock, nullptr) != 0 ||
            rwlock_rdlock(rwlock) != 0 || rwlock_unlock(rwlock) != 0 ||
            sem_init(semaphore, 0, 0) != 0 || sem_post(semaphore) != 0 ||
            sem_wait(semaphore) != 0) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& worker : workers) worker.join();
  ::dlclose(library);
  if (failures.load(std::memory_order_relaxed) != 0) {
    std::fprintf(stderr, "bionic sync stress: initialization failures=%u\n",
                 failures.load(std::memory_order_relaxed));
    return 1;
  }
  return 0;
}
