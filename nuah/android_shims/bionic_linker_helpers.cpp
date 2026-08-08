#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* This DSO is intentionally boring.  The old Android linker performs a
 * process-global lookup for a handful of fortify/property objects before
 * libhybris can apply its normal per-relocation hook.  Export only those
 * objects; never put pthread, TLS, or a second libc implementation here. */
extern "C" {
alignas(void*) unsigned char __sF[3][152]{};
std::FILE* nuah_linker_stdin __asm__("stdin") = nullptr;
std::FILE* nuah_linker_stdout __asm__("stdout") = nullptr;
std::FILE* nuah_linker_stderr __asm__("stderr") = nullptr;
unsigned long long __stack_chk_guard = 0x9e3779b97f4a7c15ULL;

void* __memset_chk(void* dst, int value, std::size_t count,
                   std::size_t capacity) {
  if (count > capacity) std::abort();
  return std::memset(dst, value, count);
}
void* __memcpy_chk(void* dst, const void* src, std::size_t count,
                   std::size_t capacity) {
  if (count > capacity) std::abort();
  return std::memcpy(dst, src, count);
}
void* __memmove_chk(void* dst, const void* src, std::size_t count,
                    std::size_t capacity) {
  if (count > capacity) std::abort();
  return std::memmove(dst, src, count);
}
char* __strcpy_chk(char* dst, const char* src, std::size_t capacity) {
  const std::size_t length = src ? std::strlen(src) : 0;
  if (!src || length + 1 > capacity) std::abort();
  return std::strcpy(dst, src);
}
char* __strcat_chk(char* dst, const char* src, std::size_t capacity) {
  const std::size_t used = dst ? std::strlen(dst) : 0;
  const std::size_t added = src ? std::strlen(src) : 0;
  if (!dst || !src || used + added + 1 > capacity) std::abort();
  return std::strcat(dst, src);
}
char* __strchr_chk(const char* text, int c, std::size_t) {
  return const_cast<char*>(text ? std::strchr(text, c) : nullptr);
}
std::size_t __strlen_chk(const char* text, std::size_t) {
  return text ? std::strlen(text) : 0;
}
char* __strncpy_chk(char* dst, const char* src, std::size_t count,
                    std::size_t capacity) {
  if (!dst || !src || count > capacity) std::abort();
  return std::strncpy(dst, src, count);
}
char* __strncpy_chk2(char* dst, const char* src, std::size_t count,
                     std::size_t capacity, std::size_t) {
  return __strncpy_chk(dst, src, count, capacity);
}
ssize_t __write_chk(int fd, const void* data, std::size_t count,
                    std::size_t capacity) {
  if (count > capacity) std::abort();
  return ::write(fd, data, count);
}
ssize_t __sendto_chk(int fd, const void* data, std::size_t count,
                     std::size_t capacity, int flags,
                     const sockaddr* address, socklen_t address_length) {
  if (count > capacity) std::abort();
  return ::sendto(fd, data, count, flags, address, address_length);
}
std::size_t __fwrite_chk(const void* data, std::size_t size,
                         std::size_t count, std::FILE* stream,
                         std::size_t) {
  return std::fwrite(data, size, count, stream);
}
std::size_t __fread_chk(void* data, std::size_t size, std::size_t count,
                        std::FILE* stream, std::size_t) {
  return std::fread(data, size, count, stream);
}
void __FD_CLR_chk(int fd, void* set, std::size_t) { FD_CLR(fd, static_cast<fd_set*>(set)); }
int __FD_ISSET_chk(int fd, const void* set, std::size_t) { return FD_ISSET(fd, static_cast<const fd_set*>(set)); }
void __FD_SET_chk(int fd, void* set, std::size_t) { FD_SET(fd, static_cast<fd_set*>(set)); }
void __assert2(const char*, int, const char*, const char*) { std::abort(); }
int* __errno() { return &errno; }
char* __gnu_strerror_r(int error, char* buffer, std::size_t length) {
  if (!buffer || !length) return buffer;
  std::snprintf(buffer, length, "%s", std::strerror(error));
  return buffer;
}
int __system_property_get(const char* key, char* value) {
  const char* result = "";
  if (key && std::strcmp(key, "ro.product.cpu.abi") == 0) result = "x86_64";
  else if (key && std::strcmp(key, "ro.build.version.sdk") == 0) result = "36";
  else if (key && std::strcmp(key, "ro.build.version.release") == 0) result = "10";
  if (value) std::strcpy(value, result);
  return static_cast<int>(std::strlen(result));
}
void android_set_abort_message(const char*) {}
}
