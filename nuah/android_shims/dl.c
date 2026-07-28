#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "nuah/android_abi_registry.h"

extern void* host_dlopen(const char*, int);
extern void* host_dlsym(void*, const char*);
extern int host_dlclose(void*);
extern char* host_dlerror(void);
extern void* host_dlvsym(void*, const char*, const char*);
asm(".symver host_dlopen,dlopen@GLIBC_2.2.5");
asm(".symver host_dlsym,dlsym@GLIBC_2.2.5");
asm(".symver host_dlclose,dlclose@GLIBC_2.2.5");
asm(".symver host_dlerror,dlerror@GLIBC_2.2.5");
asm(".symver host_dlvsym,dlvsym@GLIBC_2.2.5");

static uintptr_t parse_hex(const char* text) {
  uintptr_t value = 0;
  if (!text) return 0;
  for (; *text; ++text) {
    unsigned int digit;
    if (*text >= '0' && *text <= '9') digit = (unsigned int)(*text - '0');
    else if (*text >= 'a' && *text <= 'f') digit = (unsigned int)(*text - 'a' + 10);
    else if (*text >= 'A' && *text <= 'F') digit = (unsigned int)(*text - 'A' + 10);
    else break;
    value = (value << 4) | digit;
  }
  return value;
}

int dladdr(const void* address, Dl_info* info) {
  const char* base_text = getenv("NUAH_IMAGE_BASE");
  const char* size_text = getenv("NUAH_IMAGE_SIZE");
  const uintptr_t base = parse_hex(base_text);
  const uintptr_t size = parse_hex(size_text);
  const uintptr_t value = (uintptr_t)address;
  if (!base || !size || value < base || value >= base + size) return 0;
  info->dli_fname = "libroblox.so";
  info->dli_fbase = (void*)base;
  info->dli_sname = NULL;
  info->dli_saddr = NULL;
  return 1;
}

void* dlopen(const char* file, int flags) { return host_dlopen(file, flags); }
void* dlsym(void* handle, const char* name) {
  return host_dlsym(handle, name);
}
int dlclose(void* handle) { return host_dlclose(handle); }
char* dlerror(void) { return host_dlerror(); }
void* dlvsym(void* handle, const char* name, const char* version) {
  return host_dlvsym(handle, name, version);
}

__attribute__((constructor)) static void register_dl_abi(void) {
  static const char* symbols[] = {
      "dladdr", "dlclose", "dlerror", "dlopen", "dlsym", "dlvsym"};
  for (unsigned int i = 0; i < sizeof(symbols) / sizeof(symbols[0]); ++i) {
    nuah_android_api_register("libdl.so", symbols[i],
                              NUAH_ANDROID_API_FORWARDED);
  }
}
