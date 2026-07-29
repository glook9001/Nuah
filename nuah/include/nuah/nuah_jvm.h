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
void* nuah_jvm_jni_env(NuahJvm* jvm);

// Returns the exact function pointer recorded by Roblox through
// RegisterNatives, or NULL.  Callers must match on the complete JNI contract;
// method-name-only lookup is deliberately not provided.
void* nuah_jvm_find_registered_native(NuahJvm* jvm, const char* class_name,
                                      const char* method_name,
                                      const char* signature);

// Opaque façade objects are allocated by the same imported JNI core used for
// JNI_OnLoad.  They are deliberately not host pointers or a second fake JVM.
void* nuah_jvm_game_activity(NuahJvm* jvm);
void* nuah_jvm_key_event(NuahJvm* jvm, int keycode, int action, int repeat,
                         int scancode, unsigned int modifiers,
                         unsigned long long event_time_ms);
void* nuah_jvm_motion_event(NuahJvm* jvm, int action, int button, double x,
                            double y, double dx, double dy,
                            unsigned long long event_time_ms);

// Invoke exact GameActivity natives registered by libroblox.so.  These calls
// are kept here so callback arguments always belong to this NuahJvm.
long long nuah_jvm_initialize_game(NuahJvm* jvm, const char* package_name,
                                   const char* data_path);
int nuah_jvm_dispatch_lifecycle(NuahJvm* jvm, const char* method_name);
int nuah_jvm_dispatch_key(NuahJvm* jvm, int keycode, int action, int repeat,
                          int scancode, unsigned int modifiers,
                          unsigned long long event_time_ms);
int nuah_jvm_dispatch_motion(NuahJvm* jvm, int action, int button, double x,
                             double y, double dx, double dy,
                             unsigned long long event_time_ms);

#ifdef __cplusplus
}
#endif
