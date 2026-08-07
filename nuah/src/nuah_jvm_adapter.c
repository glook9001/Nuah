#include "nuah/nuah_jvm.h"
#include "nuah/native_window_bridge.h"

#include <stddef.h>
#include "jvm/jvm.h"

#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

extern jobject java_lang_Class_getClassLoader(JNIEnv*, jobject);
jobject (*nuah_java_facade_link_anchor)(JNIEnv*, jobject) =
    java_lang_Class_getClassLoader;
extern jobject
com_roblox_engine_jni_NativeGLJavaInterface_getDeviceStaticParams(
    JNIEnv*, jclass, va_list);
jobject (*nuah_roblox_java_facade_link_anchor)(JNIEnv*, jclass, va_list) =
    com_roblox_engine_jni_NativeGLJavaInterface_getDeviceStaticParams;

#define NUAH_CONTAINER_OF(ptr, type, member) \
  ((type*)((char*)(ptr) - offsetof(type, member)))

enum nuah_event_kind { NUAH_EVENT_NONE, NUAH_EVENT_KEY, NUAH_EVENT_MOTION };

struct nuah_event {
  jobject object;
  enum nuah_event_kind kind;
  int keycode, action, repeat, scancode;
  unsigned int modifiers;
  unsigned long long event_time_ms;
  int button;
  double x, y, dx, dy;
};

struct NuahJvm {
  struct jvm core;
  jclass (*base_find_class)(JNIEnv*, const char*);
  jint (*base_register_natives)(JNIEnv*, jclass, const JNINativeMethod*, jint);
  jint (*base_call_int)(JNIEnv*, jobject, jmethodID, ...);
  jint (*base_call_int_v)(JNIEnv*, jobject, jmethodID, va_list);
  jint (*base_call_int_a)(JNIEnv*, jobject, jmethodID, jvalue*);
  jlong (*base_call_long)(JNIEnv*, jobject, jmethodID, ...);
  jlong (*base_call_long_v)(JNIEnv*, jobject, jmethodID, va_list);
  jlong (*base_call_long_a)(JNIEnv*, jobject, jmethodID, jvalue*);
  jlong (*base_call_static_long)(JNIEnv*, jclass, jmethodID, ...);
  jlong (*base_call_static_long_v)(JNIEnv*, jclass, jmethodID, va_list);
  jlong (*base_call_static_long_a)(JNIEnv*, jclass, jmethodID, jvalue*);
  jobject activity;
  jlong native_handle;
  struct nuah_event key;
  struct nuah_event motion;
  jobject surface;
  NuahNativeWindow* surface_window;
};

static int bootstrap_trace_enabled(void) {
  const char* value = getenv("NUAH_BOOTSTRAP_TRACE");
  return value && *value;
}

static jclass nuah_find_class(JNIEnv* env, const char* name) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  jclass result = jvm->base_find_class(env, name);
  if (bootstrap_trace_enabled()) {
    fprintf(stderr, "nuah jvm: FindClass %s -> %p\n",
            name ? name : "(null)", result);
  }
  return result;
}

static jint nuah_register_natives(JNIEnv* env, jclass klass,
                                  const JNINativeMethod* methods,
                                  jint count) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  if (bootstrap_trace_enabled()) {
    fprintf(stderr, "nuah jvm: RegisterNatives class=%s count=%d\n",
            jvm_get_class_name(&jvm->core, klass), count);
  }
  return jvm->base_register_natives(env, klass, methods, count);
}

static struct jvm_object* object_for(struct NuahJvm* jvm, jobject object) {
  const uintptr_t index = (uintptr_t)object;
  if (!jvm || !index || index > sizeof(jvm->core.objects) / sizeof(jvm->core.objects[0]))
    return NULL;
  return &jvm->core.objects[index - 1];
}

static const char* method_name_for(struct NuahJvm* jvm, jmethodID method) {
  struct jvm_object* object = object_for(jvm, (jobject)method);
  return object && object->type == JVM_OBJECT_METHOD ? object->method.name.data : NULL;
}

static struct nuah_event* event_for(struct NuahJvm* jvm, jobject object) {
  if (jvm && jvm->key.object == object) return &jvm->key;
  if (jvm && jvm->motion.object == object) return &jvm->motion;
  return NULL;
}

static jint nuah_get_version(JNIEnv* env) {
  (void)env;
  return JNI_VERSION_1_6;
}

static jint nuah_call_int(JNIEnv* env, jobject object, jmethodID method, ...) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  const struct nuah_event* event = event_for(jvm, object);
  const char* name = method_name_for(jvm, method);
  if (event && name) {
    if (!strcmp(name, "getKeyCode")) return event->keycode;
    if (!strcmp(name, "getAction")) return event->action;
    if (!strcmp(name, "getRepeatCount")) return event->repeat;
    if (!strcmp(name, "getScanCode")) return event->scancode;
    if (!strcmp(name, "getMetaState")) return (jint)event->modifiers;
  }
  va_list args;
  va_start(args, method);
  const jint result = jvm->base_call_int_v
                          ? jvm->base_call_int_v(env, object, method, args)
                          : 0;
  va_end(args);
  return result;
}

static jint nuah_call_int_v(JNIEnv* env, jobject object, jmethodID method, va_list args) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  const struct nuah_event* event = event_for(jvm, object);
  const char* name = method_name_for(jvm, method);
  if (event && name) {
    if (!strcmp(name, "getKeyCode")) return event->keycode;
    if (!strcmp(name, "getAction")) return event->action;
    if (!strcmp(name, "getRepeatCount")) return event->repeat;
    if (!strcmp(name, "getScanCode")) return event->scancode;
    if (!strcmp(name, "getMetaState")) return (jint)event->modifiers;
  }
  return jvm->base_call_int_v
             ? jvm->base_call_int_v(env, object, method, args)
             : 0;
}

static jint nuah_call_int_a(JNIEnv* env, jobject object, jmethodID method, jvalue* args) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  const struct nuah_event* event = event_for(jvm, object);
  const char* name = method_name_for(jvm, method);
  if (event && name) {
    if (!strcmp(name, "getKeyCode")) return event->keycode;
    if (!strcmp(name, "getAction")) return event->action;
    if (!strcmp(name, "getRepeatCount")) return event->repeat;
    if (!strcmp(name, "getScanCode")) return event->scancode;
    if (!strcmp(name, "getMetaState")) return (jint)event->modifiers;
  }
  return jvm->base_call_int_a
             ? jvm->base_call_int_a(env, object, method, args)
             : 0;
}

static jlong nuah_call_long(JNIEnv* env, jobject object, jmethodID method, ...) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  const struct nuah_event* event = event_for(jvm, object);
  const char* name = method_name_for(jvm, method);
  if (event && name && !strcmp(name, "getEventTime"))
    return (jlong)event->event_time_ms;
  va_list args;
  va_start(args, method);
  const jlong result = jvm->base_call_long_v
                           ? jvm->base_call_long_v(env, object, method, args)
                           : 0;
  va_end(args);
  return result;
}

static jlong nuah_call_long_v(JNIEnv* env, jobject object, jmethodID method, va_list args) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  const struct nuah_event* event = event_for(jvm, object);
  const char* name = method_name_for(jvm, method);
  if (event && name && !strcmp(name, "getEventTime"))
    return (jlong)event->event_time_ms;
  return jvm->base_call_long_v
             ? jvm->base_call_long_v(env, object, method, args)
             : 0;
}

static jlong nuah_call_long_a(JNIEnv* env, jobject object, jmethodID method, jvalue* args) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  const struct nuah_event* event = event_for(jvm, object);
  const char* name = method_name_for(jvm, method);
  if (event && name && !strcmp(name, "getEventTime"))
    return (jlong)event->event_time_ms;
  return jvm->base_call_long_a
             ? jvm->base_call_long_a(env, object, method, args)
             : 0;
}

static jlong nuah_call_static_long(JNIEnv* env, jclass klass, jmethodID method, ...) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  (void)klass;
  const char* name = method_name_for(jvm, method);
  if (!name || strcmp(name, "getProcessTimestamp")) {
    va_list args;
    va_start(args, method);
    const jlong result = jvm->base_call_static_long_v
                             ? jvm->base_call_static_long_v(env, klass, method, args)
                             : 0;
    va_end(args);
    return result;
  }
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static jlong nuah_call_static_long_v(JNIEnv* env, jclass klass, jmethodID method, va_list args) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  const char* name = method_name_for(jvm, method);
  if (!name || strcmp(name, "getProcessTimestamp"))
    return jvm->base_call_static_long_v
               ? jvm->base_call_static_long_v(env, klass, method, args)
               : 0;
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static jlong nuah_call_static_long_a(JNIEnv* env, jclass klass, jmethodID method, jvalue* args) {
  struct NuahJvm* jvm = NUAH_CONTAINER_OF(env, struct NuahJvm, core.env);
  const char* name = method_name_for(jvm, method);
  if (!name || strcmp(name, "getProcessTimestamp"))
    return jvm->base_call_static_long_a
               ? jvm->base_call_static_long_a(env, klass, method, args)
               : 0;
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static jobject make_object(struct NuahJvm* jvm, const char* class_name) {
  if (!jvm || !class_name) return NULL;
  jclass klass = jvm->core.native.FindClass(&jvm->core.env, class_name);
  return klass ? jvm->core.native.AllocObject(&jvm->core.env, klass) : NULL;
}

static int class_name_matches(const char* stored, const char* requested) {
  if (!stored || !requested) return 0;
  while (*stored && *requested) {
    const char actual = *stored == '.' ? '/' : *stored;
    const char expected = *requested == '.' ? '/' : *requested;
    if (actual != expected) return 0;
    ++stored;
    ++requested;
  }
  return *stored == '\0' && *requested == '\0';
}

NuahJvm* nuah_jvm_create(void) {
  NuahJvm* jvm = calloc(1, sizeof(*jvm));
  if (!jvm) return NULL;
  jvm_init(&jvm->core);
  jvm->base_find_class = jvm->core.native.FindClass;
  jvm->base_register_natives = jvm->core.native.RegisterNatives;
  jvm->base_call_int = jvm->core.native.CallIntMethod;
  jvm->base_call_int_v = jvm->core.native.CallIntMethodV;
  jvm->base_call_int_a = jvm->core.native.CallIntMethodA;
  jvm->base_call_long = jvm->core.native.CallLongMethod;
  jvm->base_call_long_v = jvm->core.native.CallLongMethodV;
  jvm->base_call_long_a = jvm->core.native.CallLongMethodA;
  jvm->base_call_static_long = jvm->core.native.CallStaticLongMethod;
  jvm->base_call_static_long_v = jvm->core.native.CallStaticLongMethodV;
  jvm->base_call_static_long_a = jvm->core.native.CallStaticLongMethodA;
  jvm->core.native.FindClass = nuah_find_class;
  jvm->core.native.RegisterNatives = nuah_register_natives;
  jvm->core.native.GetVersion = nuah_get_version;
  jvm->core.native.CallIntMethod = nuah_call_int;
  jvm->core.native.CallIntMethodV = nuah_call_int_v;
  jvm->core.native.CallIntMethodA = nuah_call_int_a;
  jvm->core.native.CallLongMethod = nuah_call_long;
  jvm->core.native.CallLongMethodV = nuah_call_long_v;
  jvm->core.native.CallLongMethodA = nuah_call_long_a;
  jvm->core.native.CallStaticLongMethod = nuah_call_static_long;
  jvm->core.native.CallStaticLongMethodV = nuah_call_static_long_v;
  jvm->core.native.CallStaticLongMethodA = nuah_call_static_long_a;
  jvm->activity = make_object(jvm, "com/google/androidgamesdk/GameActivity");
  if (!jvm->activity) {
    jvm_release(&jvm->core);
    free(jvm);
    return NULL;
  }
  return jvm;
}

void nuah_jvm_destroy(NuahJvm* jvm) {
  if (!jvm) return;
  nuah_jvm_clear_surface(jvm);
  jvm_release(&jvm->core);
  free(jvm);
}

void* nuah_jvm_java_vm(NuahJvm* jvm) {
  return jvm ? &jvm->core.vm : NULL;
}

void* nuah_jvm_jni_env(NuahJvm* jvm) {
  return jvm ? &jvm->core.env : NULL;
}

void* nuah_jvm_find_registered_native(NuahJvm* jvm, const char* class_name,
                                      const char* method_name,
                                      const char* signature) {
  if (!jvm || !class_name || !method_name || !signature) return NULL;
  for (size_t index = 0;
       index < sizeof(jvm->core.methods) / sizeof(jvm->core.methods[0]);
       ++index) {
    const struct jvm_native_method* candidate = &jvm->core.methods[index];
    if (!candidate->function) continue;
    const char* candidate_class =
        jvm_get_class_name(&jvm->core, candidate->method.klass);
    if (candidate_class && candidate->method.name.data &&
        candidate->method.signature.data &&
        class_name_matches(candidate_class, class_name) &&
        strcmp(candidate->method.name.data, method_name) == 0 &&
        strcmp(candidate->method.signature.data, signature) == 0) {
      return candidate->function;
    }
  }
  return NULL;
}

int nuah_jvm_bind_native(NuahJvm* jvm, const char* class_name,
                         const char* method_name, const char* signature,
                         void* function) {
  if (!jvm || !class_name || !method_name || !signature || !function) return 0;
  jclass klass = jvm->core.native.FindClass(&jvm->core.env, class_name);
  if (!klass) return 0;
  const JNINativeMethod method = {
      .name = method_name, .signature = signature, .fnPtr = function};
  return jvm->core.native.RegisterNatives(&jvm->core.env, klass, &method, 1) ==
         JNI_OK;
}

void* nuah_jvm_game_activity(NuahJvm* jvm) { return jvm ? jvm->activity : NULL; }

void* nuah_jvm_key_event(NuahJvm* jvm, int keycode, int action, int repeat,
                         int scancode, unsigned int modifiers,
                         unsigned long long event_time_ms) {
  if (!jvm) return NULL;
  if (!jvm->key.object) jvm->key.object = make_object(jvm, "android/view/KeyEvent");
  jvm->key.kind = NUAH_EVENT_KEY;
  jvm->key.keycode = keycode; jvm->key.action = action; jvm->key.repeat = repeat;
  jvm->key.scancode = scancode; jvm->key.modifiers = modifiers;
  jvm->key.event_time_ms = event_time_ms;
  return jvm->key.object;
}

void* nuah_jvm_motion_event(NuahJvm* jvm, int action, int button, double x,
                            double y, double dx, double dy,
                            unsigned long long event_time_ms) {
  if (!jvm) return NULL;
  if (!jvm->motion.object) jvm->motion.object = make_object(jvm, "android/view/MotionEvent");
  jvm->motion.kind = NUAH_EVENT_MOTION;
  jvm->motion.action = action; jvm->motion.button = button;
  jvm->motion.x = x; jvm->motion.y = y; jvm->motion.dx = dx; jvm->motion.dy = dy;
  jvm->motion.event_time_ms = event_time_ms;
  return jvm->motion.object;
}

void* nuah_jvm_surface(NuahJvm* jvm, NuahNativeWindow* window) {
  if (!jvm || !window) return NULL;
  if (jvm->surface && jvm->surface_window == window) return jvm->surface;
  nuah_jvm_clear_surface(jvm);
  jvm->surface = make_object(jvm, "android/view/Surface");
  if (!jvm->surface || !nuah_native_window_alias_surface(window, jvm->surface)) {
    jvm->surface = NULL;
    return NULL;
  }
  jvm->surface_window = window;
  return jvm->surface;
}

void nuah_jvm_clear_surface(NuahJvm* jvm) {
  if (!jvm || !jvm->surface) return;
  nuah_native_window_unregister_surface(jvm->surface);
  jvm->surface = NULL;
  jvm->surface_window = NULL;
}

long long nuah_jvm_initialize_game(NuahJvm* jvm, const char* package_name,
                                   const char* data_path) {
  static const char* signature =
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
      "Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J";
  typedef jlong (*callback_t)(JNIEnv*, jobject, jstring, jstring, jstring, jobject, jbyteArray, jobject);
  callback_t callback = (callback_t)nuah_jvm_find_registered_native(
      jvm, "com/google/androidgamesdk/GameActivity", "initializeNativeCode", signature);
  if (!callback) return 0;
  JNIEnv* env = &jvm->core.env;
  jobject assets = make_object(jvm, "android/content/res/AssetManager");
  jobject configuration = make_object(jvm, "android/content/res/Configuration");
  jbyteArray saved_state = jvm->core.native.NewByteArray(env, 0);
  const char* path = data_path ? data_path : "";
  char obb_path[4096];
  char external_path[4096];
  snprintf(obb_path, sizeof(obb_path), "%s/obb", path);
  snprintf(external_path, sizeof(external_path), "%s/external", path);
  (void)package_name;
  if (bootstrap_trace_enabled()) {
    Dl_info info = {0};
    const char* object_name = "(unknown)";
    const char* symbol_name = "(unknown)";
    if (dladdr((void*)callback, &info)) {
      if (info.dli_fname) object_name = info.dli_fname;
      if (info.dli_sname) symbol_name = info.dli_sname;
    }
    fprintf(stderr,
            "nuah jvm: initialize callback=%p object=%p assets=%p saved=%p "
            "configuration=%p module=%s symbol=%s\n",
            (void*)callback, jvm->activity, assets, saved_state, configuration,
            object_name, symbol_name);
  }
  jvm->native_handle = callback(env, jvm->activity,
                  jvm->core.native.NewStringUTF(env, path),
                  jvm->core.native.NewStringUTF(env, obb_path),
                  jvm->core.native.NewStringUTF(env, external_path), assets,
                  saved_state, configuration);
  return jvm->native_handle;
}

int nuah_jvm_dispatch_lifecycle(NuahJvm* jvm, const char* method_name) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong);
  callback_t callback = (callback_t)nuah_jvm_find_registered_native(
      jvm, "com/google/androidgamesdk/GameActivity", method_name, "(J)V");
  if (!callback) return 0;
  callback(&jvm->core.env, jvm->activity, jvm->native_handle);
  return 1;
}

int nuah_jvm_dispatch_surface_created(NuahJvm* jvm, void* surface) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong, jobject);
  callback_t callback = (callback_t)nuah_jvm_find_registered_native(
      jvm, "com/google/androidgamesdk/GameActivity", "onSurfaceCreatedNative",
      "(JLandroid/view/Surface;)V");
  if (!callback || !surface) return 0;
  callback(&jvm->core.env, jvm->activity, jvm->native_handle, surface);
  return 1;
}

int nuah_jvm_dispatch_surface_changed(NuahJvm* jvm, void* surface, int format,
                                      int width, int height) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong, jobject, jint, jint, jint);
  callback_t callback = (callback_t)nuah_jvm_find_registered_native(
      jvm, "com/google/androidgamesdk/GameActivity", "onSurfaceChangedNative",
      "(JLandroid/view/Surface;III)V");
  if (!callback || !surface || width <= 0 || height <= 0) return 0;
  callback(&jvm->core.env, jvm->activity, jvm->native_handle, surface, format,
           width, height);
  return 1;
}

int nuah_jvm_dispatch_surface_destroyed(NuahJvm* jvm, void* surface) {
  typedef void (*callback_t)(JNIEnv*, jobject, jlong);
  callback_t callback = (callback_t)nuah_jvm_find_registered_native(
      jvm, "com/google/androidgamesdk/GameActivity", "onSurfaceDestroyedNative",
      "(J)V");
  if (!callback || !surface) return 0;
  callback(&jvm->core.env, jvm->activity, jvm->native_handle);
  return 1;
}

int nuah_jvm_dispatch_key(NuahJvm* jvm, int keycode, int action, int repeat,
                          int scancode, unsigned int modifiers,
                          unsigned long long event_time_ms) {
  static const char* signature = "(JLandroid/view/KeyEvent;)Z";
  typedef jboolean (*callback_t)(JNIEnv*, jobject, jlong, jobject);
  callback_t callback = (callback_t)nuah_jvm_find_registered_native(
      jvm, "com/google/androidgamesdk/GameActivity",
      action ? "onKeyDownNative" : "onKeyUpNative", signature);
  jobject event = (jobject)nuah_jvm_key_event(jvm, keycode, action, repeat, scancode,
                                               modifiers, event_time_ms);
  return callback && event && callback(&jvm->core.env, jvm->activity, jvm->native_handle, event) == JNI_TRUE;
}

int nuah_jvm_dispatch_motion(NuahJvm* jvm, int action, int button, double x,
                             double y, double dx, double dy,
                             unsigned long long event_time_ms) {
  static const char* signature = "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z";
  typedef jboolean (*callback_t)(JNIEnv*, jobject, jlong, jobject, jint, jint, jint, jint, jint,
                                 jlong, jlong, jint, jint, jint, jint, jint, jint, jfloat, jfloat);
  callback_t callback = (callback_t)nuah_jvm_find_registered_native(
      jvm, "com/google/androidgamesdk/GameActivity", "onTouchEventNative", signature);
  jobject event = (jobject)nuah_jvm_motion_event(jvm, action, button, x, y, dx, dy, event_time_ms);
  const jint action_button = (action == 0 || action == 1) ? button : 0;
  return callback && event && callback(&jvm->core.env, jvm->activity,
                                       jvm->native_handle, event, 1, 0, 0,
                                       0x1002, action, event_time_ms,
                                       event_time_ms, 0, 0, action_button,
                                       button, 0, 0, 1.0f, 1.0f) == JNI_TRUE;
}
