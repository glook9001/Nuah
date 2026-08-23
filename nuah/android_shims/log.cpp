#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nuah/bootstrap_diagnostics.h"

namespace {
NuahDiagnosticsCallbacks diagnostics_callbacks{};

bool log_enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("NUAH_ANDROID_LOG");
    return v && *v && std::strcmp(v, "0") != 0;
  }();
  return enabled;
}

void record(const char* message) {
  if (diagnostics_callbacks.record_log) {
    diagnostics_callbacks.record_log(message);
  }
}
}  // namespace

#ifndef NUAH_LIKELY
#define NUAH_LIKELY(x) (__builtin_expect(!!(x), 1))
#endif
#ifndef NUAH_UNLIKELY
#define NUAH_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#endif

extern "C" {
void nuah_log_set_diagnostics_callbacks(
    const NuahDiagnosticsCallbacks* callbacks) {
  diagnostics_callbacks =
      callbacks && callbacks->version == 1 ? *callbacks
                                           : NuahDiagnosticsCallbacks{};
}
int __android_log_write(int priority, const char* tag, const char* text) {
  if (NUAH_LIKELY(!log_enabled() && !diagnostics_callbacks.record_log)) return 0;
  if (log_enabled()) {
    std::fprintf(stderr, "[android:%d] %s: %s\n", priority, tag ? tag : "Nuah", text ? text : "");
  }
  record(text);
  return 0;
}
int __android_log_buf_write(int, int priority, const char* tag, const char* text) {
  return __android_log_write(priority, tag, text);
}
int __android_log_buf_print(int, int priority, const char* tag,
                            const char* format, ...) {
  if (NUAH_LIKELY(!log_enabled() && !diagnostics_callbacks.record_log)) return 0;
  char message[NUAH_BOOTSTRAP_TEXT_CAPACITY]{};
  va_list arguments;
  va_start(arguments, format);
  const int result =
      std::vsnprintf(message, sizeof(message), format ? format : "", arguments);
  va_end(arguments);
  __android_log_write(priority, tag, message);
  return result;
}
int __android_log_print(int priority, const char* tag, const char* format, ...) {
  if (NUAH_LIKELY(!log_enabled() && !diagnostics_callbacks.record_log)) return 0;
  char message[NUAH_BOOTSTRAP_TEXT_CAPACITY]{};
  va_list arguments;
  va_start(arguments, format);
  const int result =
      std::vsnprintf(message, sizeof(message), format ? format : "", arguments);
  va_end(arguments);
  if (log_enabled()) {
    std::fprintf(stderr, "[android:%d] %s%s%s\n", priority, tag ? tag : "",
                 tag ? ": " : "", message);
  }
  record(message);
  return result;
}
int __android_log_vprint(int priority, const char* tag, const char* format,
                         va_list arguments) {
  if (NUAH_LIKELY(!log_enabled() && !diagnostics_callbacks.record_log)) return 0;
  char message[NUAH_BOOTSTRAP_TEXT_CAPACITY]{};
  const int result =
      std::vsnprintf(message, sizeof(message), format ? format : "", arguments);
  __android_log_write(priority, tag, message);
  return result;
}
void __android_log_assert(const char* condition, const char* tag, const char* format, ...) {
  char detail[NUAH_BOOTSTRAP_TEXT_CAPACITY]{};
  if (format) {
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(detail, sizeof(detail), format, arguments);
    va_end(arguments);
  }
  char message[NUAH_BOOTSTRAP_TEXT_CAPACITY]{};
  std::snprintf(message, sizeof(message), "%s%s%s",
                condition ? condition : "assertion failed",
                detail[0] ? "; " : "", detail);
  std::fprintf(stderr, "[android:assert] %s: %s\n", tag ? tag : "Nuah",
               message);
  record(message);
}
}  // extern "C"
