#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include "nuah/android_abi_registry.h"
namespace { std::mutex m; std::unordered_map<std::string, int> r; std::unordered_map<std::string, bool> d;
std::string k(const char* a, const char* b) { return (a ? a : "<unknown>") + std::string(":") + (b ? b : "<unknown>"); } }
extern "C" void nuah_android_api_register(const char* a, const char* b, int v) { if (!a || !b || v < NUAH_ANDROID_API_IMPLEMENTED || v > NUAH_ANDROID_API_UNSUPPORTED) return; std::scoped_lock l(m); r[k(a, b)] = v; } extern "C" int nuah_android_api_disposition(const char* a, const char* b) { std::scoped_lock l(m); auto f = r.find(k(a, b)); return f == r.end() ? 0 : f->second; }
extern "C" void nuah_android_api_unsupported(const char* a, const char* b) { if (!a || !b) return; auto n = k(a, b); bool f = false; { std::scoped_lock l(m); r[n] = NUAH_ANDROID_API_UNSUPPORTED; f = d.emplace(n, true).second; } if (f) std::fprintf(stderr, "Nuah Android API unsupported: %s\n", n.c_str()); }
