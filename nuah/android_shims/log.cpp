#include <cstdarg>
#include <cstdio>

extern "C" {
int __android_log_write(int priority, const char* tag, const char* text) {
  std::fprintf(stderr, "[android:%d] %s: %s\n", priority, tag ? tag : "Nuah", text ? text : "");
  return 0;
}
int __android_log_buf_write(int, int priority, const char* tag, const char* text) {
  return __android_log_write(priority, tag, text);
}
int __android_log_print(int, const char* tag, const char* format, ...) {
  std::fputs("[android] ", stderr);
  if (tag) std::fprintf(stderr, "%s: ", tag);
  va_list arguments;
  va_start(arguments, format);
  const int result = std::vfprintf(stderr, format ? format : "", arguments);
  va_end(arguments);
  std::fputc('\n', stderr);
  return result;
}
void __android_log_assert(const char* condition, const char* tag, const char* format, ...) {
  std::fprintf(stderr, "[android:assert] %s: %s", tag ? tag : "Nuah", condition ? condition : "assertion failed");
  if (format) {
    std::fputs("; ", stderr);
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
  }
  std::fputc('\n', stderr);
}
}  // extern "C"
