#include "nuah/jni_contract.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
std::mutex mutex;
std::unordered_map<std::string, NuahJniNativeFunction> natives;
std::unordered_map<std::string, bool> missing;

std::string key(const char* class_name, const char* member,
                const char* signature) {
  return std::string(class_name ? class_name : "<null>") + "\n" +
         (member ? member : "<null>") + "\n" +
         (signature ? signature : "<null>");
}
}  // namespace

extern "C" int nuah_jni_register_native(
    const char* class_name, const char* method_name, const char* signature,
    NuahJniNativeFunction function) {
  if (!class_name || !method_name || !signature || !function) return -1;
  std::scoped_lock lock(mutex);
  const auto inserted = natives.emplace(key(class_name, method_name, signature),
                                        function);
  if (!inserted.second) inserted.first->second = function;
  return 0;
}

extern "C" NuahJniNativeFunction nuah_jni_find_native(
    const char* class_name, const char* method_name, const char* signature) {
  std::scoped_lock lock(mutex);
  const auto found = natives.find(key(class_name, method_name, signature));
  return found == natives.end() ? nullptr : found->second;
}

extern "C" NuahJniNativeFunction nuah_jni_find_native_method(
    const char* method_name, const char* signature) {
  if (!method_name || !signature) return nullptr;
  const std::string suffix = std::string("\n") + method_name + "\n" + signature;
  std::scoped_lock lock(mutex);
  for (const auto& [name, function] : natives) {
    if (name.ends_with(suffix)) return function;
  }
  return nullptr;
}

extern "C" void nuah_jni_report_missing(const char* class_name,
                                           const char* member,
                                           const char* signature) {
  const auto name = key(class_name, member, signature);
  bool first_report = false;
  {
    std::scoped_lock lock(mutex);
    first_report = missing.emplace(name, true).second;
  }
  if (first_report) {
    std::fprintf(stderr, "Nuah JNI missing: %s/%s %s\n",
                 class_name ? class_name : "<null>",
                 member ? member : "<null>", signature ? signature : "<null>");
  }
}

extern "C" size_t nuah_jni_registered_count(void) {
  std::scoped_lock lock(mutex);
  return natives.size();
}
