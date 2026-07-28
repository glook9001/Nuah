#pragma once

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NuahJniRuntime NuahJniRuntime;

NuahJniRuntime* nuah_jni_runtime_create(void);
void nuah_jni_runtime_destroy(NuahJniRuntime* runtime);
JavaVM* nuah_jni_runtime_vm(NuahJniRuntime* runtime);
JNIEnv* nuah_jni_runtime_env(NuahJniRuntime* runtime);
int nuah_jni_runtime_dispatch_key(NuahJniRuntime* runtime, int keycode,
                                  int action, int repeat, int scancode,
                                  unsigned int modifiers,
                                  unsigned long long event_time_ms);
jlong nuah_jni_runtime_initialize_game(NuahJniRuntime* runtime,
                                       const char* package_name,
                                       const char* data_path);
int nuah_jni_runtime_dispatch_lifecycle(NuahJniRuntime* runtime,
                                         const char* method_name);

#ifdef __cplusplus
}
#endif
