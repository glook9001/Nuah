#pragma once

#include "nuah/nuah_jvm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NuahNativeSession NuahNativeSession;

NuahNativeSession* nuah_native_session_create(void);
void nuah_native_session_destroy(NuahNativeSession* session);
NuahJvm* nuah_native_session_jvm(NuahNativeSession* session);
long long nuah_native_session_initialize_game(NuahNativeSession* session,
                                              const char* package_name,
                                              const char* data_path);
int nuah_native_session_dispatch_lifecycle(NuahNativeSession* session,
                                           const char* method_name);
int nuah_native_session_dispatch_key(NuahNativeSession* session, int keycode,
                                     int action, int repeat, int scancode,
                                     unsigned int modifiers,
                                     unsigned long long event_time_ms);
int nuah_native_session_dispatch_pointer(NuahNativeSession* session,
                                         int action, int button, double x,
                                         double y, double dx, double dy,
                                         unsigned long long event_time_ms);

#ifdef __cplusplus
}
#endif
