#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*NuahJniNativeFunction)(void);

/* Demand-driven subset of RegisterNatives. The registry deliberately stores
 * the exact class/name/signature triple rather than guessing Java classes. */
int nuah_jni_register_native(const char* class_name, const char* method_name,
                             const char* signature,
                             NuahJniNativeFunction function);
NuahJniNativeFunction nuah_jni_find_native(const char* class_name,
                                           const char* method_name,
                                           const char* signature);
void nuah_jni_report_missing(const char* class_name, const char* member,
                             const char* signature);
size_t nuah_jni_registered_count(void);

#ifdef __cplusplus
}
#endif
