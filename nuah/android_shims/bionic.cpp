#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <atomic>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
std::FILE __sF[3]{};
uintptr_t __stack_chk_guard = 0x9e3779b97f4a7c15ULL;
}

namespace {
struct MutexEntry { void* android = nullptr; pthread_mutex_t native{}; };
struct CondEntry { void* android = nullptr; pthread_cond_t native{}; };
struct OnceEntry { void* android = nullptr; pthread_once_t native = PTHREAD_ONCE_INIT; };
std::atomic_flag table_lock = ATOMIC_FLAG_INIT;
MutexEntry mutexes[2048];
CondEntry conditions[1024];
OnceEntry onces[1024];
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
}  // extern "C"
