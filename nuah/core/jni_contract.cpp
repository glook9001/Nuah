#include "nuah/jni_contract.h"
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
namespace {
std::mutex mutex;
std::unordered_map<std::string, NuahJniNativeFunction> natives;
std::unordered_map<std::string, bool> missing;
std::string key(const char* c, const char* m, const char* s) { return std::string(c ? c : "<null>") + "\n" + (m ? m : "<null>") + "\n" + (s ? s : "<null>"); }
}  // namespace
extern "C" int nuah_jni_register_native(const char* c, const char* m, const char* s, NuahJniNativeFunction f) {
  if (!c || !m || !s || !f) return -1;
  std::scoped_lock lock(mutex);
  natives[key(c, m, s)] = f;
  return 0;
}
extern "C" NuahJniNativeFunction nuah_jni_find_native(const char* c, const char* m, const char* s) {
  std::scoped_lock lock(mutex);
  const auto f = natives.find(key(c, m, s));
  return f == natives.end() ? nullptr : f->second;
}
extern "C" NuahJniNativeFunction nuah_jni_find_native_method(const char* m, const char* s) {
  if (!m || !s) return nullptr;
  const std::string suffix = std::string("\n") + m + "\n" + s;
  std::scoped_lock lock(mutex);
  for (const auto& [name, function] : natives) if (name.ends_with(suffix)) return function;
  return nullptr;
}
extern "C" void nuah_jni_report_missing(const char* c, const char* m, const char* s) {
  bool first = false;
  { std::scoped_lock lock(mutex); first = missing.emplace(key(c, m, s), true).second; }
  if (first) std::fprintf(stderr, "Nuah JNI missing: %s/%s %s\n", c ? c : "<null>", m ? m : "<null>", s ? s : "<null>");
}
extern "C" size_t nuah_jni_registered_count(void) { std::scoped_lock lock(mutex); return natives.size(); }
