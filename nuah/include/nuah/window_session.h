#pragma once

#include "nuah/native_window_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NuahWindowSession NuahWindowSession;

NuahWindowSession* nuah_window_session_create(int width, int height,
                                               const char* title);
void nuah_window_session_destroy(NuahWindowSession* session);
int nuah_window_session_should_close(const NuahWindowSession* session);
void nuah_window_session_pump(NuahWindowSession* session);
NuahNativeWindow* nuah_window_session_native_window(
    const NuahWindowSession* session);

/* Keep a compositor-visible host frame up while Roblox builds its first
 * scene. SDL hides the popup after a short minimum hold from the session pump
 * thread; no SDL call is made from Roblox's render thread. */
void nuah_window_session_show_loading(NuahWindowSession* session);
void nuah_window_session_hide_loading(NuahWindowSession* session);

#ifdef __cplusplus
}
#endif
