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
int nuah_native_session_dispatch_window_focus(NuahNativeSession* session,
                                              int has_focus);
void* nuah_native_session_surface(NuahNativeSession* session,
                                  NuahNativeWindow* window);
void nuah_native_session_clear_surface(NuahNativeSession* session);
int nuah_native_session_dispatch_surface_created(NuahNativeSession* session,
                                                 void* surface);
int nuah_native_session_dispatch_surface_changed(NuahNativeSession* session,
                                                 void* surface, int format,
                                                 int width, int height);
int nuah_native_session_dispatch_surface_destroyed(NuahNativeSession* session,
                                                   void* surface);
int nuah_native_session_dispatch_key(NuahNativeSession* session, int keycode,
                                     int action, int repeat, int scancode,
                                     unsigned int modifiers,
                                     unsigned long long event_time_ms);
int nuah_native_session_dispatch_pointer(NuahNativeSession* session,
                                         int action, int button, double x,
                                         double y, double dx, double dy,
                                         unsigned long long event_time_ms);
int nuah_native_session_dispatch_pointer_event(
    NuahNativeSession* session, int pointer_type, int action, int button,
    double x, double y, double dx, double dy,
    unsigned long long event_time_ms);

#ifdef __cplusplus
}
#endif
