#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "nuah/bootstrap_diagnostics.h"
namespace { NuahDiagnosticsCallbacks diagnostics_callbacks{};
inline bool env_enabled(const char* n) { const char* v = std::getenv(n); return v && *v && std::strcmp(v, "0") != 0; } bool log_enabled() { static const bool e = env_enabled("NUAH_ANDROID_LOG"); return e; }
inline void record(const char* m) { if (diagnostics_callbacks.record_log) diagnostics_callbacks.record_log(m); } }
#ifndef NUAH_LIKELY
#define NUAH_LIKELY(x) (__builtin_expect(!!(x), 1))
#endif
extern "C" {
void nuah_log_set_diagnostics_callbacks(const NuahDiagnosticsCallbacks* c) { diagnostics_callbacks = c && c->version == 1 ? *c : NuahDiagnosticsCallbacks{}; }
#define NUAH_LOG_GUARD if (NUAH_LIKELY(!log_enabled() && !diagnostics_callbacks.record_log)) return 0
int __android_log_write(int p, const char* t, const char* x) { NUAH_LOG_GUARD; if (log_enabled()) std::fprintf(stderr, "[android:%d] %s: %s\n", p, t ? t : "Nuah", x ? x : ""); record(x); return 0; }
static int log_vwrite(int p, const char* t, const char* f, va_list a) { char m[NUAH_BOOTSTRAP_TEXT_CAPACITY]{}; int r = std::vsnprintf(m, sizeof m, f ? f : "", a); __android_log_write(p, t, m); return r; }
// One-liner forwarders share the zero-alloc vsnprintf path (no heap, static buffer only).
int __android_log_buf_write(int, int p, const char* t, const char* x) { return __android_log_write(p, t, x); } int __android_log_buf_print(int, int p, const char* t, const char* f, ...) { NUAH_LOG_GUARD; va_list a; va_start(a, f); int r = log_vwrite(p, t, f, a); va_end(a); return r; }
int __android_log_print(int p, const char* t, const char* f, ...) { NUAH_LOG_GUARD; char m[NUAH_BOOTSTRAP_TEXT_CAPACITY]{}; va_list a; va_start(a, f); int r = std::vsnprintf(m, sizeof m, f ? f : "", a); va_end(a); if (log_enabled()) std::fprintf(stderr, "[android:%d] %s%s%s\n", p, t ? t : "", t ? ": " : "", m); record(m); return r; } int __android_log_vprint(int p, const char* t, const char* f, va_list a) { NUAH_LOG_GUARD; return log_vwrite(p, t, f, a); }
void __android_log_assert(const char* c, const char* t, const char* f, ...) { char d[NUAH_BOOTSTRAP_TEXT_CAPACITY]{}; if (f) { va_list a; va_start(a, f); std::vsnprintf(d, sizeof d, f, a); va_end(a); } char m[NUAH_BOOTSTRAP_TEXT_CAPACITY]{}; std::snprintf(m, sizeof m, "%s%s%s", c ? c : "assertion failed", d[0] ? "; " : "", d); std::fprintf(stderr, "[android:assert] %s: %s\n", t ? t : "Nuah", m); record(m); } }  // extern "C"
