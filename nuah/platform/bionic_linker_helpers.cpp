#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
/* Boring DSO: old linker does process-global lookup for fortify/property objects before hybris hook. Export only those; no pthread/TLS/second libc. */
extern "C" {
alignas(void*) unsigned char __sF[3][152]{}; std::FILE* nuah_linker_stdin __asm__("stdin") = nullptr; std::FILE* nuah_linker_stdout __asm__("stdout") = nullptr;
std::FILE* nuah_linker_stderr __asm__("stderr") = nullptr; unsigned long long __stack_chk_guard = 0x9e3779b97f4a7c15ULL;
#ifndef NUAH_UNLIKELY
#define NUAH_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif
#define NUAH_LCHK(ret, name, params, cond, call) ret name params { if (NUAH_UNLIKELY(cond)) std::abort(); return call; }
NUAH_LCHK(void*, __memset_chk, (void* d, int v, std::size_t n, std::size_t c), n > c, std::memset(d, v, n)) NUAH_LCHK(void*, __memcpy_chk, (void* d, const void* s, std::size_t n, std::size_t c), n > c, std::memcpy(d, s, n))
NUAH_LCHK(void*, __memmove_chk, (void* d, const void* s, std::size_t n, std::size_t c), n > c, std::memmove(d, s, n)) NUAH_LCHK(char*, __strncpy_chk, (char* d, const char* s, std::size_t n, std::size_t c), !d || !s || n > c, std::strncpy(d, s, n))
NUAH_LCHK(ssize_t, __write_chk, (int fd, const void* d, std::size_t n, std::size_t c), n > c, ::write(fd, d, n)) NUAH_LCHK(ssize_t, __sendto_chk, (int fd, const void* d, std::size_t n, std::size_t c, int f, const sockaddr* a, socklen_t l), n > c, ::sendto(fd, d, n, f, a, l))
char* __strcpy_chk(char* d, const char* s, std::size_t c) { const std::size_t n = s ? std::strlen(s) : 0; if (NUAH_UNLIKELY(!s || n + 1 > c)) std::abort(); return std::strcpy(d, s); } char* __strcat_chk(char* d, const char* s, std::size_t c) { const std::size_t n = (d ? std::strlen(d) : 0) + (s ? std::strlen(s) : 0); if (NUAH_UNLIKELY(!d || !s || n + 1 > c)) std::abort(); return std::strcat(d, s); }
std::size_t __fwrite_chk(const void* data, std::size_t size, std::size_t count, std::FILE* stream, std::size_t) { return std::fwrite(data, size, count, stream); } std::size_t __fread_chk(void* data, std::size_t size, std::size_t count, std::FILE* stream, std::size_t) { return std::fread(data, size, count, stream); }
char* __strchr_chk(const char* t, int c, std::size_t) { return const_cast<char*>(t ? std::strchr(t, c) : nullptr); } std::size_t __strlen_chk(const char* t, std::size_t) { return t ? std::strlen(t) : 0; } char* __strncpy_chk2(char* d, const char* s, std::size_t n, std::size_t c, std::size_t) { return __strncpy_chk(d, s, n, c); }
void __FD_CLR_chk(int fd, void* set, std::size_t) { FD_CLR(fd, static_cast<fd_set*>(set)); } int __FD_ISSET_chk(int fd, const void* set, std::size_t) { return FD_ISSET(fd, static_cast<const fd_set*>(set)); } void __FD_SET_chk(int fd, void* set, std::size_t) { FD_SET(fd, static_cast<fd_set*>(set)); }
void __assert2(const char*, int, const char*, const char*) { std::abort(); } int* __errno() { return &errno; } char* __gnu_strerror_r(int error, char* buffer, std::size_t length) { if (!buffer || !length) return buffer; std::snprintf(buffer, length, "%s", std::strerror(error)); return buffer; }
int __system_property_get(const char* key, char* value) { static const struct { const char* key; const char* value; } kProps[] = {{"ro.product.cpu.abi", "x86_64"}, {"ro.build.version.sdk", "36"}, {"ro.build.version.release", "10"}};
  const char* result = ""; for (const auto& p : kProps) if (key && std::strcmp(key, p.key) == 0) { result = p.value; break; } if (value) std::strcpy(value, result); return static_cast<int>(std::strlen(result)); }
void android_set_abort_message(const char*) {} }
