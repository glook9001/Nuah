#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "jvm/jvm.h"
#include "nuah/native_window_bridge.h"
#include "nuah/nuah_jvm.h"
extern jobject java_lang_Class_getClassLoader(JNIEnv*, jobject);
jobject (*nuah_java_facade_link_anchor)(JNIEnv*, jobject) = java_lang_Class_getClassLoader;
extern jobject com_roblox_engine_jni_NativeGLJavaInterface_getDeviceStaticParams(JNIEnv*, jclass, va_list);
jobject (*nuah_roblox_java_facade_link_anchor)(JNIEnv*, jclass, va_list) = com_roblox_engine_jni_NativeGLJavaInterface_getDeviceStaticParams;
#define NUAH_CONTAINER_OF(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))
struct nuah_event { jobject object; int keycode, action, repeat, scancode, button; unsigned int modifiers; unsigned long long event_time_ms; double x, y, dx, dy; };
struct NuahJvm {
  struct jvm core;
  jclass (*base_find_class)(JNIEnv*, const char*);
  jint (*base_register_natives)(JNIEnv*, jclass, const JNINativeMethod*, jint);
  jint (*base_call_int)(JNIEnv*, jobject, jmethodID, ...); jint (*base_call_int_v)(JNIEnv*, jobject, jmethodID, va_list); jint (*base_call_int_a)(JNIEnv*, jobject, jmethodID, jvalue*);
  jlong (*base_call_long)(JNIEnv*, jobject, jmethodID, ...); jlong (*base_call_long_v)(JNIEnv*, jobject, jmethodID, va_list); jlong (*base_call_long_a)(JNIEnv*, jobject, jmethodID, jvalue*);
  jlong (*base_call_static_long)(JNIEnv*, jclass, jmethodID, ...); jlong (*base_call_static_long_v)(JNIEnv*, jclass, jmethodID, va_list); jlong (*base_call_static_long_a)(JNIEnv*, jclass, jmethodID, jvalue*);
  jobject activity; jlong native_handle; struct nuah_event key, motion; jobject surface; NuahNativeWindow* surface_window;
};
static int bootstrap_trace_enabled(void) { const char* v = getenv("NUAH_BOOTSTRAP_TRACE"); return v && *v; }
static jclass nuah_find_class(JNIEnv* env, const char* name) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  jclass result = jvm->base_find_class(env, name);
  if (bootstrap_trace_enabled()) { fprintf(stderr, "nuah jvm: FindClass %s -> %p\n", name ? name : "(null)", result); }
  return result;
}
static jint nuah_register_natives(JNIEnv* env, jclass klass, const JNINativeMethod* methods, jint count) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  if (bootstrap_trace_enabled()) { fprintf(stderr, "nuah jvm: RegisterNatives class=%s count=%d\n", jvm_get_class_name(&jvm->core, klass), count); }
  return jvm->base_register_natives(env, klass, methods, count);
}
static struct jvm_object* object_for(struct NuahJvm* jvm, jobject object) {
  const uintptr_t i = (uintptr_t)object;
  if (!jvm || !i || i > sizeof(jvm->core.objects) / sizeof(jvm->core.objects[0])) return NULL;
  return &jvm->core.objects[i - 1];
}
static const char* method_name_for(struct NuahJvm* jvm, jmethodID method) {
  struct jvm_object* o = object_for(jvm, (jobject)method);
  return o && o->type == JVM_OBJECT_METHOD ? o->method.name.data : NULL;
}
static struct nuah_event* event_for(struct NuahJvm* jvm, jobject object) {
  if (jvm && jvm->key.object == object) return &jvm->key;
  if (jvm && jvm->motion.object == object) return &jvm->motion;
  return NULL;
}
static jint nuah_get_version(JNIEnv* env) { (void)env; return JNI_VERSION_1_6; }
static int event_int_property(const struct nuah_event* event, const char* name, jint* out) {
  if (!event || !name || !out) return 0;
#define NUAH_INT_CASE(prop, field) if (!strcmp(name, prop)) { *out = (jint)event->field; return 1; }
  NUAH_INT_CASE("getKeyCode", keycode) NUAH_INT_CASE("getAction", action) NUAH_INT_CASE("getRepeatCount", repeat) NUAH_INT_CASE("getScanCode", scancode) NUAH_INT_CASE("getMetaState", modifiers)
#undef NUAH_INT_CASE
  return 0;
}
static jlong current_process_timestamp(void) { struct timespec n; clock_gettime(CLOCK_REALTIME, &n); return (jlong)n.tv_sec * 1000 + n.tv_nsec / 1000000; }
/* V/A/DOTS variants share fast-path preamble; only the tail call differs. */
#define NUAH_WRAP_INT_VA(name, argdecl, base) \
  static jint name(JNIEnv* env, jobject object, jmethodID method, argdecl args) { \
    struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env); jint val = 0; \
    if (event_int_property(event_for(jvm, object), method_name_for(jvm, method), &val)) return val; \
    return jvm->base ? jvm->base(env, object, method, args) : 0; }
#define NUAH_WRAP_LONG_VA(name, argdecl, base) \
  static jlong name(JNIEnv* env, jobject object, jmethodID method, argdecl args) { \
    struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env); const struct nuah_event* event = event_for(jvm, object); const char* name_ = method_name_for(jvm, method); \
    if (event && name_ && !strcmp(name_, "getEventTime")) return (jlong)event->event_time_ms; \
    return jvm->base ? jvm->base(env, object, method, args) : 0; }
#define NUAH_WRAP_STATIC_LONG_VA(name, argdecl, base) \
  static jlong name(JNIEnv* env, jclass klass, jmethodID method, argdecl args) { \
    struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env); const char* name_ = method_name_for(jvm, method); \
    if (name_ && !strcmp(name_, "getProcessTimestamp")) return current_process_timestamp(); \
    return jvm->base ? jvm->base(env, klass, method, args) : 0; }
#define NUAH_WRAP_INT_DOTS(name, base_v) static jint name(JNIEnv* env, jobject object, jmethodID method, ...) { struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env); jint v = 0; if (event_int_property(event_for(jvm, object), method_name_for(jvm, method), &v)) return v; va_list a; va_start(a, method); const jint r = jvm->base_v ? jvm->base_v(env, object, method, a) : 0; va_end(a); return r; }
#define NUAH_WRAP_LONG_DOTS(name, base_v) static jlong name(JNIEnv* env, jobject object, jmethodID method, ...) { struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env); const struct nuah_event* e = event_for(jvm, object); const char* n = method_name_for(jvm, method); if (e && n && !strcmp(n, "getEventTime")) return (jlong)e->event_time_ms; va_list a; va_start(a, method); const jlong r = jvm->base_v ? jvm->base_v(env, object, method, a) : 0; va_end(a); return r; }
#define NUAH_WRAP_STATIC_LONG_DOTS(name, base_v) static jlong name(JNIEnv* env, jclass klass, jmethodID method, ...) { struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env); const char* n = method_name_for(jvm, method); if (n && !strcmp(n, "getProcessTimestamp")) return current_process_timestamp(); va_list a; va_start(a, method); const jlong r = jvm->base_v ? jvm->base_v(env, klass, method, a) : 0; va_end(a); return r; }
NUAH_WRAP_INT_DOTS(nuah_call_int, base_call_int_v)
NUAH_WRAP_INT_VA(nuah_call_int_v, va_list, base_call_int_v)
NUAH_WRAP_INT_VA(nuah_call_int_a, jvalue*, base_call_int_a)
NUAH_WRAP_LONG_DOTS(nuah_call_long, base_call_long_v)
NUAH_WRAP_LONG_VA(nuah_call_long_v, va_list, base_call_long_v)
NUAH_WRAP_LONG_VA(nuah_call_long_a, jvalue*, base_call_long_a)
NUAH_WRAP_STATIC_LONG_DOTS(nuah_call_static_long, base_call_static_long_v)
NUAH_WRAP_STATIC_LONG_VA(nuah_call_static_long_v, va_list, base_call_static_long_v)
NUAH_WRAP_STATIC_LONG_VA(nuah_call_static_long_a, jvalue*, base_call_static_long_a)
#undef NUAH_WRAP_INT_VA
#undef NUAH_WRAP_LONG_VA
#undef NUAH_WRAP_STATIC_LONG_VA
#undef NUAH_WRAP_INT_DOTS
#undef NUAH_WRAP_LONG_DOTS
#undef NUAH_WRAP_STATIC_LONG_DOTS
static jobject make_object(struct NuahJvm* jvm, const char* class_name) {
  if (!jvm || !class_name) return NULL;
  jclass k = jvm->core.native.FindClass(&jvm->core.env, class_name);
  return k ? jvm->core.native.AllocObject(&jvm->core.env, k) : NULL;
}
static int class_name_matches(const char* s, const char* r) {
  if (!s || !r) return 0;
  while (*s && *r) { const char a = *s == '.' ? '/' : *s, b = *r == '.' ? '/' : *r; if (a != b) return 0; ++s; ++r; }
  return *s == '\0' && *r == '\0';
}
NuahJvm* nuah_jvm_create(void) {
  NuahJvm* jvm = calloc(1, sizeof(*jvm));
  if (!jvm) return NULL;
  jvm_init(&jvm->core);
#define NUAH_HOOK(native_field, base_field, impl) jvm->base_field = jvm->core.native.native_field; jvm->core.native.native_field = impl;
  NUAH_HOOK(FindClass, base_find_class, nuah_find_class) NUAH_HOOK(RegisterNatives, base_register_natives, nuah_register_natives) NUAH_HOOK(CallIntMethod, base_call_int, nuah_call_int) NUAH_HOOK(CallIntMethodV, base_call_int_v, nuah_call_int_v) NUAH_HOOK(CallIntMethodA, base_call_int_a, nuah_call_int_a)
  NUAH_HOOK(CallLongMethod, base_call_long, nuah_call_long) NUAH_HOOK(CallLongMethodV, base_call_long_v, nuah_call_long_v) NUAH_HOOK(CallLongMethodA, base_call_long_a, nuah_call_long_a) NUAH_HOOK(CallStaticLongMethod, base_call_static_long, nuah_call_static_long) NUAH_HOOK(CallStaticLongMethodV, base_call_static_long_v, nuah_call_static_long_v) NUAH_HOOK(CallStaticLongMethodA, base_call_static_long_a, nuah_call_static_long_a)
#undef NUAH_HOOK
  jvm->core.native.GetVersion = nuah_get_version;
  jvm->activity = make_object(jvm, "com/google/androidgamesdk/GameActivity");
  if (!jvm->activity) { jvm_release(&jvm->core); free(jvm); return NULL; }
  return jvm;
}
void nuah_jvm_destroy(NuahJvm* jvm) { if (!jvm) return; nuah_jvm_clear_surface(jvm); jvm_release(&jvm->core); free(jvm); }
void* nuah_jvm_java_vm(NuahJvm* jvm) { return jvm ? &jvm->core.vm : NULL; }
void* nuah_jvm_jni_env(NuahJvm* jvm) { return jvm ? &jvm->core.env : NULL; }
void* nuah_jvm_find_registered_native(NuahJvm* jvm, const char* class_name, const char* method_name, const char* signature) {
  if (!jvm || !class_name || !method_name || !signature) return NULL;
  for (size_t i = 0; i < sizeof(jvm->core.methods) / sizeof(jvm->core.methods[0]); ++i) {
    const struct jvm_native_method* c = &jvm->core.methods[i];
    if (!c->function) continue;
    const char* cc = jvm_get_class_name(&jvm->core, c->method.klass);
    if (cc && c->method.name.data && c->method.signature.data && class_name_matches(cc, class_name) && !strcmp(c->method.name.data, method_name) && !strcmp(c->method.signature.data, signature)) return c->function;
  }
  return NULL;
}
int nuah_jvm_bind_native(NuahJvm* jvm, const char* class_name, const char* method_name, const char* signature, void* function) {
  if (!jvm || !class_name || !method_name || !signature || !function) return 0;
  jclass klass = jvm->core.native.FindClass(&jvm->core.env, class_name);
  if (!klass) return 0;
  const JNINativeMethod method = {.name = method_name, .signature = signature, .fnPtr = function};
  return jvm->core.native.RegisterNatives(&jvm->core.env, klass, &method, 1) == JNI_OK;
}
void* nuah_jvm_game_activity(NuahJvm* jvm) { return jvm ? jvm->activity : NULL; }
void* nuah_jvm_key_event(NuahJvm* jvm, int keycode, int action, int repeat, int scancode, unsigned int modifiers, unsigned long long event_time_ms) {
  if (!jvm) return NULL;
  if (!jvm->key.object) jvm->key.object = make_object(jvm, "android/view/KeyEvent");
  jvm->key.keycode = keycode; jvm->key.action = action; jvm->key.repeat = repeat; jvm->key.scancode = scancode; jvm->key.modifiers = modifiers; jvm->key.event_time_ms = event_time_ms;
  return jvm->key.object;
}
void* nuah_jvm_motion_event(NuahJvm* jvm, int action, int button, double x, double y, double dx, double dy, unsigned long long event_time_ms) {
  if (!jvm) return NULL;
  if (!jvm->motion.object) jvm->motion.object = make_object(jvm, "android/view/MotionEvent");
  jvm->motion.action = action; jvm->motion.button = button; jvm->motion.x = x; jvm->motion.y = y; jvm->motion.dx = dx; jvm->motion.dy = dy; jvm->motion.event_time_ms = event_time_ms;
  return jvm->motion.object;
}
void* nuah_jvm_surface(NuahJvm* jvm, NuahNativeWindow* window) {
  if (!jvm || !window) return NULL;
  if (jvm->surface && jvm->surface_window == window) return jvm->surface;
  nuah_jvm_clear_surface(jvm);
  jvm->surface = make_object(jvm, "android/view/Surface");
  if (!jvm->surface || !nuah_native_window_alias_surface(window, jvm->surface)) { jvm->surface = NULL; return NULL; }
  jvm->surface_window = window;
  return jvm->surface;
}
void nuah_jvm_clear_surface(NuahJvm* jvm) { if (!jvm || !jvm->surface) return; nuah_native_window_unregister_surface(jvm->surface); jvm->surface = NULL; jvm->surface_window = NULL; }
/* Exact JNI contract (class+name+sig) in one place; callers supply suffix only. */
#define NUAH_GAME_NATIVE(jvm_, name_, sig_, type_) ((type_)nuah_jvm_find_registered_native(jvm_, "com/google/androidgamesdk/GameActivity", name_, sig_))
static void trace_init_callback(void* callback, void* activity, void* assets, void* saved, void* configuration) {
  if (!bootstrap_trace_enabled()) return;
  Dl_info info = {0};
  const char* obj = "(unknown)", * sym = "(unknown)";
  if (dladdr(callback, &info)) { if (info.dli_fname) obj = info.dli_fname; if (info.dli_sname) sym = info.dli_sname; }
  fprintf(stderr, "nuah jvm: initialize callback=%p object=%p assets=%p saved=%p configuration=%p module=%s symbol=%s\n", callback, activity, assets, saved, configuration, obj, sym);
}
long long nuah_jvm_initialize_game(NuahJvm* jvm, const char* package_name, const char* data_path) {
  static const char* signature = "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J";
  typedef jlong (*callback_t)(JNIEnv*, jobject, jstring, jstring, jstring, jobject, jbyteArray, jobject); callback_t callback = NUAH_GAME_NATIVE(jvm, "initializeNativeCode", signature, callback_t);
  if (!callback) return 0;
  JNIEnv* env = &jvm->core.env;
  jobject assets = make_object(jvm, "android/content/res/AssetManager");
  jobject configuration = make_object(jvm, "android/content/res/Configuration");
  jbyteArray saved_state = jvm->core.native.NewByteArray(env, 0);
  const char* path = data_path ? data_path : "";
  char obb_path[4096], external_path[4096];
  snprintf(obb_path, sizeof(obb_path), "%s/obb", path); snprintf(external_path, sizeof(external_path), "%s/external", path);
  (void)package_name;
  trace_init_callback((void*)callback, jvm->activity, assets, saved_state, configuration);
  jvm->native_handle = callback(env, jvm->activity, jvm->core.native.NewStringUTF(env, path), jvm->core.native.NewStringUTF(env, obb_path), jvm->core.native.NewStringUTF(env, external_path), assets, saved_state, configuration);
  return jvm->native_handle;
}
int nuah_jvm_dispatch_lifecycle(NuahJvm* jvm, const char* method_name) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong); callback_t cb = NUAH_GAME_NATIVE(jvm, method_name, "(J)V", callback_t);
  if (!cb) return 0;
  cb(&jvm->core.env, jvm->activity, jvm->native_handle); return 1;
}
int nuah_jvm_dispatch_window_focus(NuahJvm* jvm, int has_focus) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong, jboolean); callback_t cb = NUAH_GAME_NATIVE(jvm, "onWindowFocusChangedNative", "(JZ)V", callback_t);
  if (!cb) return 0;
  cb(&jvm->core.env, jvm->activity, jvm->native_handle, has_focus ? JNI_TRUE : JNI_FALSE); return 1;
}
int nuah_jvm_dispatch_surface_created(NuahJvm* jvm, void* surface) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong, jobject); callback_t cb = NUAH_GAME_NATIVE(jvm, "onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", callback_t);
  if (!cb || !surface) return 0;
  cb(&jvm->core.env, jvm->activity, jvm->native_handle, surface); return 1;
}
int nuah_jvm_dispatch_surface_changed(NuahJvm* jvm, void* surface, int format, int width, int height) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong, jobject, jint, jint, jint); callback_t cb = NUAH_GAME_NATIVE(jvm, "onSurfaceChangedNative", "(JLandroid/view/Surface;III)V", callback_t);
  if (!cb || !surface || width <= 0 || height <= 0) return 0;
  cb(&jvm->core.env, jvm->activity, jvm->native_handle, surface, format, width, height); return 1;
}
int nuah_jvm_dispatch_surface_destroyed(NuahJvm* jvm, void* surface) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong); callback_t cb = NUAH_GAME_NATIVE(jvm, "onSurfaceDestroyedNative", "(J)V", callback_t);
  if (!cb || !surface) return 0;
  cb(&jvm->core.env, jvm->activity, jvm->native_handle); return 1;
}
int nuah_jvm_dispatch_key(NuahJvm* jvm, int keycode, int action, int repeat, int scancode, unsigned int modifiers, unsigned long long event_time_ms) {
  static const char* signature = "(JLandroid/view/KeyEvent;)Z"; typedef jboolean (*callback_t)(JNIEnv*, jobject, jlong, jobject);
  const int down = action != 0; /* Nuah 1=down/0=up; Android ACTION_DOWN=0/ACTION_UP=1. */
  callback_t cb = NUAH_GAME_NATIVE(jvm, down ? "onKeyDownNative" : "onKeyUpNative", signature, callback_t);
  jobject e = (jobject)nuah_jvm_key_event(jvm, keycode, down ? 0 : 1, repeat, scancode, modifiers, event_time_ms);
  return cb && e && cb(&jvm->core.env, jvm->activity, jvm->native_handle, e) == JNI_TRUE;
}
int nuah_jvm_dispatch_motion(NuahJvm* jvm, int action, int button, double x, double y, double dx, double dy, unsigned long long event_time_ms) {
  static const char* signature = "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z"; typedef jboolean (*callback_t)(JNIEnv*, jobject, jlong, jobject, jint, jint, jint, jint, jint, jlong, jlong, jint, jint, jint, jint, jint, jint, jfloat, jfloat);
  callback_t cb = NUAH_GAME_NATIVE(jvm, "onTouchEventNative", signature, callback_t);
  jobject e = (jobject)nuah_jvm_motion_event(jvm, action, button, x, y, dx, dy, event_time_ms);
  const jint ab = (action == 0 || action == 1) ? button : 0;
  return cb && e && cb(&jvm->core.env, jvm->activity, jvm->native_handle, e, 1, 0, 0, 0x1002, action, event_time_ms, event_time_ms, 0, 0, ab, button, 0, 0, 1.0f, 1.0f) == JNI_TRUE;
}
int nuah_jvm_dispatch_pointer(NuahJvm* jvm, int pointer_type, int action, int button, double x, double y, double dx, double dy, unsigned long long event_time_ms) {
  (void)pointer_type;
  return nuah_jvm_dispatch_motion(jvm, action, button, x, y, dx, dy, event_time_ms);
}
