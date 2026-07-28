#include "nuah/android_abi_registry.h"

#include <cassert>

int main() {
  nuah_android_api_register("libvulkan.so", "vkCreateAndroidSurfaceKHR",
                            NUAH_ANDROID_API_TRANSLATED);
  assert(nuah_android_api_disposition(
             "libvulkan.so", "vkCreateAndroidSurfaceKHR") ==
         NUAH_ANDROID_API_TRANSLATED);

  nuah_android_api_unsupported("libandroid.so", "ALooper_pollOnce");
  assert(nuah_android_api_disposition("libandroid.so", "ALooper_pollOnce") ==
         NUAH_ANDROID_API_UNSUPPORTED);
  assert(nuah_android_api_disposition("libandroid.so", "missing") == 0);
  return 0;
}
