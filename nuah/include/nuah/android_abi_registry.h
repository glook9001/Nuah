#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum NuahAndroidApiDisposition {
  NUAH_ANDROID_API_IMPLEMENTED = 1,
  NUAH_ANDROID_API_TRANSLATED = 2,
  NUAH_ANDROID_API_FORWARDED = 3,
  NUAH_ANDROID_API_UNSUPPORTED = 4,
};

void nuah_android_api_register(const char* soname,
                               const char* symbol,
                               int disposition);
int nuah_android_api_disposition(const char* soname, const char* symbol);
void nuah_android_api_unsupported(const char* soname, const char* symbol);

#ifdef __cplusplus
}
#endif
