#include "nuah/android_abi_registry.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
std::mutex registry_mutex;
std::unordered_map<std::string, int> registry;
std::unordered_map<std::string, bool> reported;

std::string key(const char* soname, const char* symbol) {
  return std::string(soname ? soname : "<unknown>") + ":" +
         (symbol ? symbol : "<unknown>");
}
}

extern "C" void nuah_android_api_register(const char* soname,
                                            const char* symbol,
                                            int disposition) {
  if (!soname || !symbol || disposition < NUAH_ANDROID_API_IMPLEMENTED ||
      disposition > NUAH_ANDROID_API_UNSUPPORTED) {
    return;
  }
  std::scoped_lock lock(registry_mutex);
  registry[key(soname, symbol)] = disposition;
}

extern "C" int nuah_android_api_disposition(const char* soname,
                                              const char* symbol) {
  std::scoped_lock lock(registry_mutex);
  const auto found = registry.find(key(soname, symbol));
  return found == registry.end() ? 0 : found->second;
}

extern "C" void nuah_android_api_unsupported(const char* soname,
                                               const char* symbol) {
  if (!soname || !symbol) return;
  const auto name = key(soname, symbol);
  bool first = false;
  {
    std::scoped_lock lock(registry_mutex);
    registry[name] = NUAH_ANDROID_API_UNSUPPORTED;
    first = reported.emplace(name, true).second;
  }
  if (first) {
    std::fprintf(stderr, "Nuah Android API unsupported: %s\n", name.c_str());
  }
}
