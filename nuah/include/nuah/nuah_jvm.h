#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NuahJvm NuahJvm;

NuahJvm* nuah_jvm_create(void);
void nuah_jvm_destroy(NuahJvm* jvm);

// Returns the ABI-level JavaVM reference expected by an Android JNI_OnLoad.
// It is opaque here so android2gnulinux's C JNI types never leak into Nuah's
// C++ sources, which use Android's official libnativehelper declarations.
void* nuah_jvm_java_vm(NuahJvm* jvm);

// Returns the exact function pointer recorded by Roblox through
// RegisterNatives, or NULL.  Callers must match on the complete JNI contract;
// method-name-only lookup is deliberately not provided.
void* nuah_jvm_find_registered_native(NuahJvm* jvm, const char* class_name,
                                      const char* method_name,
                                      const char* signature);

#ifdef __cplusplus
}
#endif
