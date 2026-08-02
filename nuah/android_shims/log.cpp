#include <cstdarg>
#include <cstdio>

#include "nuah/bootstrap_diagnostics.h"

namespace {
NuahDiagnosticsCallbacks diagnostics_callbacks{};

void record(const char* message) {
  if (diagnostics_callbacks.record_log) {
    diagnostics_callbacks.record_log(message);
  }
}
}  // namespace

extern "C" {
void nuah_log_set_diagnostics_callbacks(
    const NuahDiagnosticsCallbacks* callbacks) {
  diagnostics_callbacks =
      callbacks && callbacks->version == 1 ? *callbacks
                                           : NuahDiagnosticsCallbacks{};
}
int __android_log_write(int priority, const char* tag, const char* text) {
  std::fprintf(stderr, "[android:%d] %s: %s\n", priority, tag ? tag : "Nuah", text ? text : "");
  record(text);
  return 0;
}
int __android_log_buf_write(int, int priority, const char* tag, const char* text) {
  return __android_log_write(priority, tag, text);
}
int __android_log_buf_print(int, int priority, const char* tag,
                            const char* format, ...) {
  char message[NUAH_BOOTSTRAP_TEXT_CAPACITY]{};
  va_list arguments;
  va_start(arguments, format);
  const int result =
      std::vsnprintf(message, sizeof(message), format ? format : "", arguments);
  va_end(arguments);
  __android_log_write(priority, tag, message);
  return result;
}
int __android_log_print(int, const char* tag, const char* format, ...) {
  char message[NUAH_BOOTSTRAP_TEXT_CAPACITY]{};
  va_list arguments;
  va_start(arguments, format);
  const int result =
      std::vsnprintf(message, sizeof(message), format ? format : "", arguments);
  va_end(arguments);
  std::fprintf(stderr, "[android] %s%s%s\n", tag ? tag : "",
               tag ? ": " : "", message);
  record(message);
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
