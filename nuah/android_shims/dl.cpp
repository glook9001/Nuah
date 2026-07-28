#include <dlfcn.h>

#include "nuah/android_abi_registry.h"

extern "C" void* host_dlopen(const char*, int);
extern "C" void* host_dlsym(void*, const char*);
extern "C" int host_dlclose(void*);
extern "C" char* host_dlerror();
extern "C" void* host_dlvsym(void*, const char*, const char*);
asm(".symver host_dlopen,dlopen@GLIBC_2.2.5");
asm(".symver host_dlsym,dlsym@GLIBC_2.2.5");
asm(".symver host_dlclose,dlclose@GLIBC_2.2.5");
asm(".symver host_dlerror,dlerror@GLIBC_2.2.5");
asm(".symver host_dlvsym,dlvsym@GLIBC_2.2.5");

#include <cstdlib>
#include <cstdint>

extern "C" int dladdr(const void* address, Dl_info* info) {
  const char* base_text = std::getenv("NUAH_IMAGE_BASE");
  const char* size_text = std::getenv("NUAH_IMAGE_SIZE");
  const auto base = base_text ? std::strtoull(base_text, nullptr, 16) : 0;
  const auto size = size_text ? std::strtoull(size_text, nullptr, 16) : 0;
  const auto value = reinterpret_cast<std::uintptr_t>(address);
  if (!base || !size || value < base || value >= base + size) return 0;
  info->dli_fname = "libroblox.so";
  info->dli_fbase = reinterpret_cast<void*>(base);
  info->dli_sname = nullptr;
  info->dli_saddr = nullptr;
  return 1;
}

extern "C" void* dlopen(const char* file, int flags) {
  return host_dlopen(file, flags);
}
extern "C" void* dlsym(void* handle, const char* name) {
  return host_dlsym(handle, name);
}
extern "C" int dlclose(void* handle) {
  return host_dlclose(handle);
}
extern "C" char* dlerror() {
  return host_dlerror();
}
extern "C" void* dlvsym(void* handle, const char* name, const char* version) {
  return host_dlvsym(handle, name, version);
}

__attribute__((constructor)) static void register_dl_abi() {
  static constexpr const char* symbols[] = {
      "dladdr", "dlclose", "dlerror", "dlopen", "dlsym", "dlvsym"};
  for (const char* symbol : symbols) {
    nuah_android_api_register("libdl.so", symbol,
                              NUAH_ANDROID_API_FORWARDED);
  }
}
