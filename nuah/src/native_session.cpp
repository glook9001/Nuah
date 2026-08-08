#include "nuah/native_session.h"

struct NuahNativeSession {
  NuahJvm* jvm = nullptr;
};

extern "C" NuahNativeSession* nuah_native_session_create(void) {
  auto* session = new NuahNativeSession;
  session->jvm = nuah_jvm_create();
  if (!session->jvm) {
    delete session;
    return nullptr;
  }
  return session;
}

extern "C" void nuah_native_session_destroy(NuahNativeSession* session) {
  if (!session) return;
  nuah_jvm_destroy(session->jvm);
  delete session;
}

extern "C" NuahJvm* nuah_native_session_jvm(NuahNativeSession* session) {
  return session ? session->jvm : nullptr;
}

extern "C" long long nuah_native_session_initialize_game(
    NuahNativeSession* session, const char* package_name, const char* data_path) {
  return session ? nuah_jvm_initialize_game(session->jvm, package_name, data_path) : 0;
}

extern "C" int nuah_native_session_dispatch_lifecycle(
    NuahNativeSession* session, const char* method_name) {
  return session ? nuah_jvm_dispatch_lifecycle(session->jvm, method_name) : 0;
}

extern "C" int nuah_native_session_dispatch_window_focus(
    NuahNativeSession* session, int has_focus) {
  return session ? nuah_jvm_dispatch_window_focus(session->jvm, has_focus) : 0;
}

extern "C" void* nuah_native_session_surface(NuahNativeSession* session,
                                               NuahNativeWindow* window) {
  return session ? nuah_jvm_surface(session->jvm, window) : nullptr;
}

extern "C" void nuah_native_session_clear_surface(NuahNativeSession* session) {
  if (session) nuah_jvm_clear_surface(session->jvm);
}

extern "C" int nuah_native_session_dispatch_surface_created(
    NuahNativeSession* session, void* surface) {
  return session ? nuah_jvm_dispatch_surface_created(session->jvm, surface) : 0;
}

extern "C" int nuah_native_session_dispatch_surface_changed(
    NuahNativeSession* session, void* surface, int format, int width, int height) {
  return session ? nuah_jvm_dispatch_surface_changed(session->jvm, surface, format,
                                                      width, height)
                 : 0;
}

extern "C" int nuah_native_session_dispatch_surface_destroyed(
    NuahNativeSession* session, void* surface) {
  return session ? nuah_jvm_dispatch_surface_destroyed(session->jvm, surface) : 0;
}

extern "C" int nuah_native_session_dispatch_key(
    NuahNativeSession* session, int keycode, int action, int repeat,
    int scancode, unsigned int modifiers, unsigned long long event_time_ms) {
  return session ? nuah_jvm_dispatch_key(session->jvm, keycode, action, repeat,
                                         scancode, modifiers, event_time_ms)
                 : 0;
}

extern "C" int nuah_native_session_dispatch_pointer(
    NuahNativeSession* session, int action, int button, double x, double y,
    double dx, double dy, unsigned long long event_time_ms) {
  return session ? nuah_jvm_dispatch_motion(session->jvm, action, button, x, y,
                                            dx, dy, event_time_ms)
                 : 0;
}

extern "C" int nuah_native_session_dispatch_pointer_event(
    NuahNativeSession* session, int pointer_type, int action, int button,
    double x, double y, double dx, double dy,
    unsigned long long event_time_ms) {
  return session ? nuah_jvm_dispatch_pointer(
                       session->jvm, pointer_type, action, button, x, y, dx,
                       dy, event_time_ms)
                 : 0;
}
