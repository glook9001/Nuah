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

#ifdef __cplusplus
}
#endif
