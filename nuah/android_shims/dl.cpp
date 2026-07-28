#include <dlfcn.h>

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
