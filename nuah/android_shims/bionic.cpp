#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <atomic>
#include <cstdarg>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

extern "C" {
std::FILE __sF[3]{};
uintptr_t __stack_chk_guard = 0x9e3779b97f4a7c15ULL;
}

namespace {
struct MutexEntry { void* android = nullptr; pthread_mutex_t native{}; };
struct CondEntry { void* android = nullptr; pthread_cond_t native{}; };
struct OnceEntry { void* android = nullptr; pthread_once_t native = PTHREAD_ONCE_INIT; };
struct AttrEntry { void* android = nullptr; pthread_attr_t native{}; };
struct RwlockEntry { void* android = nullptr; pthread_rwlock_t native{}; };
std::atomic_flag table_lock = ATOMIC_FLAG_INIT;
MutexEntry mutexes[2048];
CondEntry conditions[1024];
OnceEntry onces[1024];
AttrEntry attributes[256];
RwlockEntry rwlocks[1024];
void lock_table() { while (table_lock.test_and_set(std::memory_order_acquire)) {} }
void unlock_table() { table_lock.clear(std::memory_order_release); }
template <typename T> T host(const char* name) { return reinterpret_cast<T>(::dlsym(RTLD_NEXT, name)); }
MutexEntry* mutex_for(void* object) {
  lock_table();
  for (auto& entry : mutexes) if (entry.android == object) { unlock_table(); return &entry; }
  for (auto& entry : mutexes) if (!entry.android) {
    entry.android = object;
    host<int (*)(pthread_mutex_t*, const pthread_mutexattr_t*)>("pthread_mutex_init")(&entry.native, nullptr);
    unlock_table(); return &entry;
  }
  unlock_table(); return nullptr;
}
CondEntry* cond_for(void* object) {
  lock_table();
  for (auto& entry : conditions) if (entry.android == object) { unlock_table(); return &entry; }
  for (auto& entry : conditions) if (!entry.android) {
    entry.android = object;
    host<int (*)(pthread_cond_t*, const pthread_condattr_t*)>("pthread_cond_init")(&entry.native, nullptr);
    unlock_table(); return &entry;
  }
  unlock_table(); return nullptr;
}
OnceEntry* once_for(void* object) {
  lock_table();
  for (auto& entry : onces) if (entry.android == object) { unlock_table(); return &entry; }
  for (auto& entry : onces) if (!entry.android) {
    entry.android = object;
    entry.native = PTHREAD_ONCE_INIT;
    unlock_table(); return &entry;
  }
  unlock_table(); return nullptr;
}
AttrEntry* attr_for(void* object) {
  if (!object) return nullptr;
  lock_table();
  for (auto& entry : attributes) {
    if (entry.android == object) {
      unlock_table();
      return &entry;
    }
  }
  for (auto& entry : attributes) {
    if (!entry.android) {
      entry.android = object;
      host<int (*)(pthread_attr_t*)>("pthread_attr_init")(&entry.native);
      unlock_table();
      return &entry;
    }
  }
  unlock_table();
  return nullptr;
}
RwlockEntry* rwlock_for(void* object) {
  if (!object) return nullptr;
  lock_table();
  for (auto& entry : rwlocks) {
    if (entry.android == object) {
      unlock_table();
      return &entry;
    }
  }
  for (auto& entry : rwlocks) {
    if (!entry.android) {
      entry.android = object;
      host<int (*)(pthread_rwlock_t*, const pthread_rwlockattr_t*)>(
          "pthread_rwlock_init")(&entry.native, nullptr);
      unlock_table();
      return &entry;
    }
  }
  unlock_table();
  return nullptr;
}
}

extern "C" {
void* memcpy(void* destination, const void* source, size_t length) {
  return host<void* (*)(void*, const void*, size_t)>("memcpy")(
      destination, source, length);
}
void* memmove(void* destination, const void* source, size_t length) {
  return host<void* (*)(void*, const void*, size_t)>("memmove")(
      destination, source, length);
}
void* memset(void* destination, int value, size_t length) {
  return host<void* (*)(void*, int, size_t)>("memset")(destination, value,
                                                         length);
}
void* __memset_chk(void* destination, int value, size_t length,
                   size_t destination_size) {
  if (length > destination_size) std::abort();
  return memset(destination, value, length);
}
void* __memcpy_chk(void* destination, const void* source, size_t length,
                   size_t destination_size) {
  if (length > destination_size) std::abort();
  return memcpy(destination, source, length);
}
void* __memmove_chk(void* destination, const void* source, size_t length,
                    size_t destination_size) {
  if (length > destination_size) std::abort();
  return memmove(destination, source, length);
}
int memcmp(const void* left, const void* right, size_t length) {
  return host<int (*)(const void*, const void*, size_t)>("memcmp")(
      left, right, length);
}
extern "C" const void* nuah_memchr(const void* data, int value, size_t length)
    __asm__("memchr");
const void* nuah_memchr(const void* data, int value, size_t length) {
  return host<const void* (*)(const void*, int, size_t)>("memchr")(
      data, value, length);
}
size_t strlen(const char* text) {
  return host<size_t (*)(const char*)>("strlen")(text);
}
size_t strnlen(const char* text, size_t length) {
  return host<size_t (*)(const char*, size_t)>("strnlen")(text, length);
}
int strcmp(const char* left, const char* right) {
  return host<int (*)(const char*, const char*)>("strcmp")(left, right);
}
int strncmp(const char* left, const char* right, size_t length) {
  return host<int (*)(const char*, const char*, size_t)>("strncmp")(
      left, right, length);
}
char* strcpy(char* destination, const char* source) {
  return host<char* (*)(char*, const char*)>("strcpy")(destination, source);
}
char* __strcpy_chk(char* destination, const char* source,
                   size_t destination_size) {
  const size_t length = strlen(source) + 1;
  if (length > destination_size) std::abort();
  return strcpy(destination, source);
}
char* strncpy(char* destination, const char* source, size_t length) {
  return host<char* (*)(char*, const char*, size_t)>("strncpy")(
      destination, source, length);
}
char* strcat(char* destination, const char* source) {
  return host<char* (*)(char*, const char*)>("strcat")(destination, source);
}
char* __strcat_chk(char* destination, const char* source,
                   size_t destination_size) {
  const size_t length = strlen(destination) + strlen(source) + 1;
  if (length > destination_size) std::abort();
  return strcat(destination, source);
}
char* strncat(char* destination, const char* source, size_t length) {
  return host<char* (*)(char*, const char*, size_t)>("strncat")(
      destination, source, length);
}
extern "C" const char* nuah_strchr(const char* text, int value)
    __asm__("strchr");
const char* nuah_strchr(const char* text, int value) {
  return host<const char* (*)(const char*, int)>("strchr")(text, value);
}
extern "C" const char* nuah_strrchr(const char* text, int value)
    __asm__("strrchr");
const char* nuah_strrchr(const char* text, int value) {
  return host<const char* (*)(const char*, int)>("strrchr")(text, value);
}
extern "C" const char* nuah_strstr(const char* text, const char* needle)
    __asm__("strstr");
const char* nuah_strstr(const char* text, const char* needle) {
  return host<const char* (*)(const char*, const char*)>("strstr")(
      text, needle);
}
int strcasecmp(const char* left, const char* right) {
  return host<int (*)(const char*, const char*)>("strcasecmp")(left, right);
}
int strncasecmp(const char* left, const char* right, size_t length) {
  return host<int (*)(const char*, const char*, size_t)>("strncasecmp")(
      left, right, length);
}
char* strerror(int error) {
  return host<char* (*)(int)>("strerror")(error);
}
int close(int fd) {
  return host<int (*)(int)>("close")(fd);
}
ssize_t read(int fd, void* data, size_t count) {
  return host<ssize_t (*)(int, void*, size_t)>("read")(fd, data, count);
}
ssize_t __read_chk(int fd, void* data, size_t count, size_t data_size) {
  if (count > data_size) std::abort();
  return read(fd, data, count);
}
ssize_t write(int fd, const void* data, size_t count) {
  return host<ssize_t (*)(int, const void*, size_t)>("write")(fd, data, count);
}
ssize_t pread(int fd, void* data, size_t count, off_t offset) {
  return host<ssize_t (*)(int, void*, size_t, off_t)>("pread")(
      fd, data, count, offset);
}
ssize_t pwrite(int fd, const void* data, size_t count, off_t offset) {
  return host<ssize_t (*)(int, const void*, size_t, off_t)>("pwrite")(
      fd, data, count, offset);
}
ssize_t writev(int fd, const iovec* vectors, int count) {
  return host<ssize_t (*)(int, const iovec*, int)>("writev")(
      fd, vectors, count);
}
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
  return host<int (*)(const char*, int, mode_t)>("open")(path, flags, mode);
}
int __open_2(const char* path, int flags) {
  return host<int (*)(const char*, int)>("__open_2")(path, flags);
}
int access(const char* path, int mode) {
  return host<int (*)(const char*, int)>("access")(path, mode);
}
int fchmod(int fd, mode_t mode) {
  return host<int (*)(int, mode_t)>("fchmod")(fd, mode);
}
int fchown(int fd, uid_t owner, gid_t group) {
  return host<int (*)(int, uid_t, gid_t)>("fchown")(fd, owner, group);
}
int fcntl(int fd, int command, ...) {
  uintptr_t argument = 0;
  switch (command) {
    case F_GETFD:
    case F_GETFL:
    case F_GETOWN:
      break;
    default: {
      va_list arguments;
      va_start(arguments, command);
      argument = va_arg(arguments, uintptr_t);
      va_end(arguments);
      break;
    }
  }
  return host<int (*)(int, int, uintptr_t)>("fcntl")(fd, command, argument);
}
int fstat(int fd, struct stat* value) {
  return host<int (*)(int, struct stat*)>("fstat")(fd, value);
}
int lstat(const char* path, struct stat* value) {
  return host<int (*)(const char*, struct stat*)>("lstat")(path, value);
}
int stat(const char* path, struct stat* value) {
  return host<int (*)(const char*, struct stat*)>("stat")(path, value);
}
int fsync(int fd) {
  return host<int (*)(int)>("fsync")(fd);
}
int ftruncate(int fd, off_t length) {
  return host<int (*)(int, off_t)>("ftruncate")(fd, length);
}
off_t lseek(int fd, off_t offset, int origin) {
  return host<off_t (*)(int, off_t, int)>("lseek")(fd, offset, origin);
}
int mkdir(const char* path, mode_t mode) {
  return host<int (*)(const char*, mode_t)>("mkdir")(path, mode);
}
void* mmap(void* address, size_t length, int protection, int flags, int fd,
           off_t offset) {
  return host<void* (*)(void*, size_t, int, int, int, off_t)>("mmap")(
      address, length, protection, flags, fd, offset);
}
int mprotect(void* address, size_t length, int protection) {
  return host<int (*)(void*, size_t, int)>("mprotect")(
      address, length, protection);
}
int munmap(void* address, size_t length) {
  return host<int (*)(void*, size_t)>("munmap")(address, length);
}
ssize_t readlink(const char* path, char* destination, size_t length) {
  return host<ssize_t (*)(const char*, char*, size_t)>("readlink")(
      path, destination, length);
}
ssize_t __readlink_chk(const char* path, char* destination, size_t length,
                       size_t destination_size) {
  if (length > destination_size) std::abort();
  return readlink(path, destination, length);
}
int unlink(const char* path) {
  return host<int (*)(const char*)>("unlink")(path);
}
int __poll_chk(pollfd* descriptors, nfds_t count, int timeout,
               size_t descriptors_size) {
  if (count > descriptors_size / sizeof(*descriptors)) std::abort();
  return host<int (*)(pollfd*, nfds_t, int)>("poll")(
      descriptors, count, timeout);
}
extern "C" size_t nuah_fread_chk(void* destination, size_t size, size_t count,
                                 std::FILE* stream, size_t destination_size)
    __asm__("__fread_chk");
size_t nuah_fread_chk(void* destination, size_t size, size_t count,
                      std::FILE* stream, size_t destination_size) {
  if (size != 0 && count > destination_size / size) std::abort();
  return host<size_t (*)(void*, size_t, size_t, std::FILE*)>("fread")(
      destination, size, count, stream);
}
char* __strncpy_chk(char* destination, const char* source, size_t count,
                    size_t destination_size) {
  if (count > destination_size) std::abort();
  return strncpy(destination, source, count);
}
int __vsnprintf_chk(char* destination, size_t size, int, size_t destination_size,
                    const char* format, va_list arguments) {
  if (size > destination_size) std::abort();
  return host<int (*)(char*, size_t, const char*, va_list)>("vsnprintf")(
      destination, size, format, arguments);
}
int __vsprintf_chk(char* destination, int, size_t destination_size,
                   const char* format, va_list arguments) {
  const int result =
      host<int (*)(char*, size_t, const char*, va_list)>("vsnprintf")(
          destination, destination_size, format, arguments);
  if (result < 0 || static_cast<size_t>(result) >= destination_size) {
    std::abort();
  }
  return result;
}
void __assert(const char* file, int line, const char* expression) {
  std::fprintf(stderr, "[bionic] assertion %s:%d: %s\n",
               file ? file : "?", line, expression ? expression : "?");
  std::abort();
}
[[noreturn]] void __stack_chk_fail() {
  std::fprintf(stderr, "[bionic] stack corruption detected\n");
  std::abort();
}
extern "C" size_t nuah_ctype_get_mb_cur_max()
    __asm__("__ctype_get_mb_cur_max");
size_t nuah_ctype_get_mb_cur_max() {
  return host<size_t (*)()>("__ctype_get_mb_cur_max")();
}
extern "C" cmsghdr* nuah_cmsg_nxthdr(msghdr* message, cmsghdr* current)
    __asm__("__cmsg_nxthdr");
cmsghdr* nuah_cmsg_nxthdr(msghdr* message, cmsghdr* current) {
  return host<cmsghdr* (*)(msghdr*, cmsghdr*)>("__cmsg_nxthdr")(
      message, current);
}
void __cxa_finalize(void* dso) {
  host<void (*)(void*)>("__cxa_finalize")(dso);
}
int __register_atfork(void (*prepare)(void), void (*parent)(void),
                      void (*child)(void), void* dso) {
  return host<int (*)(void (*)(void), void (*)(void), void (*)(void), void*)>(
      "__register_atfork")(prepare, parent, child, dso);
}
int pthread_once(pthread_once_t* object, void (*function)(void)) {
  auto* entry = once_for(object);
  return entry ? host<int (*)(pthread_once_t*, void (*)(void))>("pthread_once")(
                    &entry->native, function)
               : ENOMEM;
}
int __cxa_atexit(void (*function)(void*), void* argument, void* dso) {
  return host<int (*)(void (*)(void*), void*, void*)>("__cxa_atexit")(
      function, argument, dso);
}
int __cxa_thread_atexit_impl(void (*function)(void*), void* argument,
                             void* dso) {
  return host<int (*)(void (*)(void*), void*, void*)>(
      "__cxa_thread_atexit_impl")(function, argument, dso);
}
void __FD_CLR_chk(int, void*, size_t) {}
int __FD_ISSET_chk(int, const void*, size_t) { return 0; }
void __FD_SET_chk(int, void*, size_t) {}
void __assert2(const char* file, int line, const char* function, const char* expression) {
  std::fprintf(stderr, "[bionic] assertion %s:%d %s: %s\n", file ? file : "?", line, function ? function : "?", expression ? expression : "?");
}
int* __errno() { return &errno; }
char* __gnu_strerror_r(int error, char* buffer, size_t length) {
  if (buffer && length) std::snprintf(buffer, length, "%s", std::strerror(error));
  return buffer;
}
char* __strchr_chk(const char* text, int character, size_t) { return const_cast<char*>(std::strchr(text, character)); }
size_t __strlen_chk(const char* text, size_t) { return std::strlen(text); }
char* __strncpy_chk2(char* destination, const char* source, size_t count, size_t, size_t) { return std::strncpy(destination, source, count); }
int __system_property_get(const char* key, char* value) {
  const char* result = "";
  if (key && std::strcmp(key, "ro.product.cpu.abi") == 0) result = "x86_64";
  else if (key && std::strcmp(key, "ro.build.version.sdk") == 0) result = "35";
  if (value) std::strcpy(value, result);
  return static_cast<int>(std::strlen(result));
}
void android_set_abort_message(const char*) {}
size_t __fwrite_chk(const void* data, size_t size, size_t count, std::FILE* stream, size_t) { return std::fwrite(data, size, count, stream); }
ssize_t __write_chk(int fd, const void* data, size_t count, size_t) { return ::write(fd, data, count); }
ssize_t __sendto_chk(int fd, const void* data, size_t count, size_t, int flags, const sockaddr* address, socklen_t length) {
  return ::sendto(fd, data, count, flags, address, length);
}
// Android's pthread_mutex_t/pthread_cond_t layout is ABI-incompatible with
// glibc. Keep host synchronization objects out-of-line and use the Android
// object address purely as Nuah's stable key.
int pthread_mutex_init(pthread_mutex_t* object, const pthread_mutexattr_t*) { return mutex_for(object) ? 0 : ENOMEM; }
int pthread_mutex_destroy(pthread_mutex_t*) { return 0; }
int pthread_mutex_lock(pthread_mutex_t* object) { auto* entry = mutex_for(object); return entry ? host<int (*)(pthread_mutex_t*)>("pthread_mutex_lock")(&entry->native) : ENOMEM; }
int pthread_mutex_trylock(pthread_mutex_t* object) { auto* entry = mutex_for(object); return entry ? host<int (*)(pthread_mutex_t*)>("pthread_mutex_trylock")(&entry->native) : ENOMEM; }
int pthread_mutex_unlock(pthread_mutex_t* object) { auto* entry = mutex_for(object); return entry ? host<int (*)(pthread_mutex_t*)>("pthread_mutex_unlock")(&entry->native) : EINVAL; }
int pthread_mutexattr_init(pthread_mutexattr_t*) { return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t*) { return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t*, int) { return 0; }
int pthread_cond_init(pthread_cond_t* object, const pthread_condattr_t*) { return cond_for(object) ? 0 : ENOMEM; }
int pthread_cond_destroy(pthread_cond_t*) { return 0; }
int pthread_cond_signal(pthread_cond_t* object) { auto* entry = cond_for(object); return entry ? host<int (*)(pthread_cond_t*)>("pthread_cond_signal")(&entry->native) : ENOMEM; }
int pthread_cond_broadcast(pthread_cond_t* object) { auto* entry = cond_for(object); return entry ? host<int (*)(pthread_cond_t*)>("pthread_cond_broadcast")(&entry->native) : ENOMEM; }
int pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex) { auto* c = cond_for(condition); auto* m = mutex_for(mutex); return c && m ? host<int (*)(pthread_cond_t*, pthread_mutex_t*)>("pthread_cond_wait")(&c->native, &m->native) : ENOMEM; }
int pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex, const timespec* timeout) { auto* c = cond_for(condition); auto* m = mutex_for(mutex); return c && m ? host<int (*)(pthread_cond_t*, pthread_mutex_t*, const timespec*)>("pthread_cond_timedwait")(&c->native, &m->native, timeout) : ENOMEM; }
int pthread_condattr_init(pthread_condattr_t*) { return 0; }
int pthread_condattr_destroy(pthread_condattr_t*) { return 0; }
int pthread_condattr_setclock(pthread_condattr_t*, clockid_t) { return 0; }
int pthread_attr_init(pthread_attr_t* object) {
  return attr_for(object) ? 0 : ENOMEM;
}
int pthread_attr_destroy(pthread_attr_t* object) {
  lock_table();
  for (auto& entry : attributes) {
    if (entry.android == object) {
      const int result =
          host<int (*)(pthread_attr_t*)>("pthread_attr_destroy")(
              &entry.native);
      entry.android = nullptr;
      unlock_table();
      return result;
    }
  }
  unlock_table();
  return EINVAL;
}
int pthread_attr_getstack(const pthread_attr_t* object, void** address,
                          size_t* size) {
  auto* entry = attr_for(const_cast<pthread_attr_t*>(object));
  return entry ? host<int (*)(const pthread_attr_t*, void**, size_t*)>(
                     "pthread_attr_getstack")(&entry->native, address, size)
               : EINVAL;
}
int pthread_attr_setdetachstate(pthread_attr_t* object, int state) {
  auto* entry = attr_for(object);
  return entry ? host<int (*)(pthread_attr_t*, int)>(
                     "pthread_attr_setdetachstate")(&entry->native, state)
               : EINVAL;
}
int pthread_attr_setschedparam(pthread_attr_t* object,
                               const sched_param* parameters) {
  auto* entry = attr_for(object);
  return entry
             ? host<int (*)(pthread_attr_t*, const sched_param*)>(
                   "pthread_attr_setschedparam")(&entry->native, parameters)
             : EINVAL;
}
int pthread_attr_setstacksize(pthread_attr_t* object, size_t size) {
  auto* entry = attr_for(object);
  return entry ? host<int (*)(pthread_attr_t*, size_t)>(
                     "pthread_attr_setstacksize")(&entry->native, size)
               : EINVAL;
}
int pthread_create(pthread_t* thread, const pthread_attr_t* object,
                   void* (*start)(void*), void* argument) {
  auto* entry = object ? attr_for(const_cast<pthread_attr_t*>(object)) : nullptr;
  return host<int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*),
                      void*)>("pthread_create")(
      thread, entry ? &entry->native : nullptr, start, argument);
}
int pthread_getattr_np(pthread_t thread, pthread_attr_t* object) {
  auto* entry = attr_for(object);
  return entry ? host<int (*)(pthread_t, pthread_attr_t*)>(
                     "pthread_getattr_np")(thread, &entry->native)
               : EINVAL;
}
int pthread_detach(pthread_t thread) {
  return host<int (*)(pthread_t)>("pthread_detach")(thread);
}
int pthread_equal(pthread_t left, pthread_t right) {
  return host<int (*)(pthread_t, pthread_t)>("pthread_equal")(left, right);
}
[[noreturn]] void pthread_exit(void* value) {
  host<void (*)(void*)>("pthread_exit")(value);
  __builtin_unreachable();
}
int pthread_getschedparam(pthread_t thread, int* policy,
                          sched_param* parameters) {
  return host<int (*)(pthread_t, int*, sched_param*)>(
      "pthread_getschedparam")(thread, policy, parameters);
}
void* pthread_getspecific(pthread_key_t key) {
  return host<void* (*)(pthread_key_t)>("pthread_getspecific")(key);
}
int pthread_join(pthread_t thread, void** value) {
  return host<int (*)(pthread_t, void**)>("pthread_join")(thread, value);
}
int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
  return host<int (*)(pthread_key_t*, void (*)(void*))>(
      "pthread_key_create")(key, destructor);
}
int pthread_key_delete(pthread_key_t key) {
  return host<int (*)(pthread_key_t)>("pthread_key_delete")(key);
}
int pthread_kill(pthread_t thread, int signal) {
  return host<int (*)(pthread_t, int)>("pthread_kill")(thread, signal);
}
pthread_t pthread_self() {
  return host<pthread_t (*)()>("pthread_self")();
}
int pthread_setname_np(pthread_t thread, const char* name) {
  return host<int (*)(pthread_t, const char*)>("pthread_setname_np")(
      thread, name);
}
int pthread_setschedparam(pthread_t thread, int policy,
                          const sched_param* parameters) {
  return host<int (*)(pthread_t, int, const sched_param*)>(
      "pthread_setschedparam")(thread, policy, parameters);
}
int pthread_setspecific(pthread_key_t key, const void* value) {
  return host<int (*)(pthread_key_t, const void*)>("pthread_setspecific")(
      key, value);
}
int pthread_sigmask(int operation, const sigset_t* set, sigset_t* old_set) {
  return host<int (*)(int, const sigset_t*, sigset_t*)>("pthread_sigmask")(
      operation, set, old_set);
}
int pthread_rwlock_init(pthread_rwlock_t* object,
                        const pthread_rwlockattr_t*) {
  return rwlock_for(object) ? 0 : ENOMEM;
}
int pthread_rwlock_destroy(pthread_rwlock_t* object) {
  lock_table();
  for (auto& entry : rwlocks) {
    if (entry.android == object) {
      const int result =
          host<int (*)(pthread_rwlock_t*)>("pthread_rwlock_destroy")(
              &entry.native);
      entry.android = nullptr;
      unlock_table();
      return result;
    }
  }
  unlock_table();
  return EINVAL;
}
int pthread_rwlock_rdlock(pthread_rwlock_t* object) {
  auto* entry = rwlock_for(object);
  return entry ? host<int (*)(pthread_rwlock_t*)>("pthread_rwlock_rdlock")(
                     &entry->native)
               : EINVAL;
}
int pthread_rwlock_wrlock(pthread_rwlock_t* object) {
  auto* entry = rwlock_for(object);
  return entry ? host<int (*)(pthread_rwlock_t*)>("pthread_rwlock_wrlock")(
                     &entry->native)
               : EINVAL;
}
int pthread_rwlock_unlock(pthread_rwlock_t* object) {
  auto* entry = rwlock_for(object);
  return entry ? host<int (*)(pthread_rwlock_t*)>("pthread_rwlock_unlock")(
                     &entry->native)
               : EINVAL;
}
}  // extern "C"
