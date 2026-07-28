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

#ifdef __cplusplus
}
#endif
