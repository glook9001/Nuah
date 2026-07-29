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
