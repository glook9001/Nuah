#include "nuah/vendoring_guard.h"
#include <cstdio>
#include <cstring>
namespace {
int fail(const char* message) {
  std::fprintf(stderr, "vendoring guard test: %s\n", message);
  return 1;
}
int check_banned(const char* path) {
  if (!nuah_path_is_banned_system_path(path)) return fail("banned path accepted");
  return 0;
}
int check_clean(const char* path) {
  if (nuah_path_is_banned_system_path(path)) return fail("clean path rejected");
  return 0;
}
}  // namespace
int main() {
  if (check_banned("/usr/local/lib64/art/libart.so")) return 1;
  if (check_banned("/usr/local/bin/dex2oat")) return 1;
  if (check_banned("/usr/local")) return 1;
  if (check_clean(nullptr)) return 1;
  if (check_clean("")) return 1;
  if (check_clean("/usr/lib64/libc.so.6")) return 1;
  if (check_clean("/lib64/ld-linux-x86-64.so.2")) return 1;
  if (check_clean("/usr/localfoo/bar.so")) return 1;
  if (check_clean("usr/local/lib.so")) return 1;
  if (check_clean("/home/pepe/Documents/nuah/build/nuah")) return 1;
  char offender[1024];
  if (nuah_vendoring_guard_scan(offender, sizeof(offender)) != 0) {
    std::fprintf(stderr, "vendoring guard test: test binary leaks banned path: %s\n", offender);
    return 1;
  }
  if (std::strlen(offender) != 0) return fail("offender not empty on clean scan");
  std::puts("vendoring guard test: clean");
  return 0;
}
