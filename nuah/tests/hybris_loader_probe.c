#include <dlfcn.h>
#include <stdio.h>

typedef void* (*android_dlopen_fn)(const char*, int);
typedef int (*android_dlclose_fn)(void*);
typedef char* (*android_dlerror_fn)(void);

int main(int argc, char** argv) {
  if (argc != 2) return 64;
  void* common = dlopen("libhybris-common.so", RTLD_NOW | RTLD_LOCAL);
  if (!common) {
    fprintf(stderr, "cannot open libhybris: %s\n", dlerror());
    return 1;
  }
  android_dlopen_fn android_dlopen =
      (android_dlopen_fn)dlsym(common, "android_dlopen");
  android_dlclose_fn android_dlclose =
      (android_dlclose_fn)dlsym(common, "android_dlclose");
  android_dlerror_fn android_dlerror =
      (android_dlerror_fn)dlsym(common, "android_dlerror");
  if (!android_dlopen || !android_dlclose || !android_dlerror) return 2;
  void* module = android_dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!module) {
    fprintf(stderr, "android_dlopen failed: %s\n", android_dlerror());
    return 3;
  }
  return android_dlclose(module) == 0 ? 0 : 4;
}
