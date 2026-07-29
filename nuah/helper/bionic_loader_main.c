#include <dlfcn.h>
#include <stdio.h>

#include "jni_facade.h"

/*
 * This program is deliberately built by the Android NDK, not the host
 * compiler.  It is started by API-36 linker64 and therefore both it and the
 * Roblox image use the extracted Android libc rather than Nuah callbacks.
 */
int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: nuah-bionic-loader <libroblox.so>\n");
    return 64;
  }
  void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!module) {
    const char* error = dlerror();
    fprintf(stderr, "bionic dlopen failed: %s\n", error ? error : "unknown error");
    return 1;
  }
  fprintf(stderr, "bionic dlopen succeeded: %s\n", argv[1]);
  const int jni_status = nuah_jni_invoke_onload(module);
  if (jni_status != 0) return jni_status;
  return dlclose(module) == 0 ? 0 : 2;
}
