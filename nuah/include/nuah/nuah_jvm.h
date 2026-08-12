#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NuahJvm NuahJvm;
typedef struct NuahNativeWindow NuahNativeWindow;

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
int nuah_jvm_bind_native(NuahJvm* jvm, const char* class_name,
                         const char* method_name, const char* signature,
                         void* function);

// Opaque façade objects are allocated by the same imported JNI core used for
// JNI_OnLoad.  They are deliberately not host pointers or a second fake JVM.
void* nuah_jvm_game_activity(NuahJvm* jvm);
// Create the ATL application object and seed only the process state required
// by MainGameActivity. The full RobloxApplication.onCreate is intentionally
// not called: it assumes Android services that Nuah does not provide.
int nuah_jvm_dispatch_application_create(NuahJvm* jvm);
// Deliver the real MainGameActivity.onCreate(Bundle) once the Roblox native
// library has installed its JNI methods. This runs the app-owned bootstrap
// (AppManager/user/session setup) instead of reimplementing it in Nuah.
int nuah_jvm_dispatch_activity_create(NuahJvm* jvm);
// GameActivity.onCreate stores the handle returned by initializeNativeCode in
// its static field. Capture that handle so later lifecycle/input calls use the
// same native session rather than creating a second one.
int nuah_jvm_capture_native_handle(NuahJvm* jvm);
void* nuah_jvm_key_event(NuahJvm* jvm, int keycode, int action, int repeat,
                         int scancode, unsigned int modifiers,
                         unsigned long long event_time_ms);
void* nuah_jvm_motion_event(NuahJvm* jvm, int action, int button, double x,
                            double y, double dx, double dy,
                            unsigned long long event_time_ms);
void* nuah_jvm_surface(NuahJvm* jvm, NuahNativeWindow* window);
void nuah_jvm_clear_surface(NuahJvm* jvm);

// Invoke exact GameActivity natives registered by libroblox.so.  These calls
// are kept here so callback arguments always belong to this NuahJvm.
long long nuah_jvm_initialize_game(NuahJvm* jvm, const char* package_name,
                                   const char* data_path);
int nuah_jvm_dispatch_lifecycle(NuahJvm* jvm, const char* method_name);
/* Mirror Android's Activity window-focus transition so GameActivity can
 * notify Roblox and request focus for its SurfaceView after realization. */
int nuah_jvm_dispatch_window_focus(NuahJvm* jvm, int has_focus);
int nuah_jvm_dispatch_surface_created(NuahJvm* jvm, void* surface);
int nuah_jvm_dispatch_surface_changed(NuahJvm* jvm, void* surface,
                                      int format, int width, int height);
/* Replay the Java SurfaceView holder callback when an older ATL native
 * provider does not emit it itself.  The method posts the callback onto the
 * provider's Android/GTK UI queue, matching Android's SurfaceHolder order. */
int nuah_jvm_dispatch_surface_view_lifecycle(NuahJvm* jvm, int width,
                                             int height);
int nuah_jvm_dispatch_surface_destroyed(NuahJvm* jvm, void* surface);
// action is Nuah's host convention: 1=key down, 0=key up. The implementation
// converts it to Android KeyEvent.ACTION_DOWN/ACTION_UP before invoking the
// GameActivity callback.
int nuah_jvm_dispatch_key(NuahJvm* jvm, int keycode, int action, int repeat,
                          int scancode, unsigned int modifiers,
                          unsigned long long event_time_ms);
int nuah_jvm_dispatch_motion(NuahJvm* jvm, int action, int button, double x,
                             double y, double dx, double dy,
                             unsigned long long event_time_ms);
/* NativeInputInterface uses a separate PC mouse path in the Android client.
 * Keep the old MotionEvent entry point for compatibility, but let the SDL
 * bridge identify motion/button/wheel events so the real Roblox methods can
 * receive them without pretending a mouse is a touchscreen. */
enum NuahPointerEventType {
  NUAH_POINTER_MOTION = 2,
  NUAH_POINTER_BUTTON = 3,
  NUAH_POINTER_WHEEL = 4,
};
int nuah_jvm_dispatch_pointer(NuahJvm* jvm, int pointer_type, int action,
                              int button, double x, double y, double dx,
                              double dy, unsigned long long event_time_ms);
/* Query Roblox's own mouse-lock state.  Sober uses this native callback to
 * decide when SurfaceView should request/release pointer capture; Nuah uses
 * the same state to synchronize SDL relative-mouse mode. */
int nuah_jvm_is_mouse_locked_center(NuahJvm* jvm);

#ifdef __cplusplus
}
#endif
