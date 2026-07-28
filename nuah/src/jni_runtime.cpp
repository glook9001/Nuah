#include "nuah/jni_runtime.h"
#include "nuah/jni_contract.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>

namespace {
struct Handle {
  std::string value;
  int kind = 0;
  int keycode = 0;
  int action = 0;
  int repeat = 0;
  int scancode = 0;
  unsigned int modifiers = 0;
  unsigned long long event_time_ms = 0;
};

thread_local NuahJniRuntime* active_runtime = nullptr;

Handle* handle(void* value) { return static_cast<Handle*>(value); }
jobject object_handle(const char* value) {
  auto* object = new Handle;
  object->value = value ? value : "";
  return reinterpret_cast<jobject>(object);
}
jclass class_handle(const char* value) {
  auto* object = new Handle;
  object->value = value ? value : "";
  return reinterpret_cast<jclass>(object);
}
jmethodID method_handle(const char* name, const char* signature) {
  auto* method = new Handle;
  method->value = std::string(name ? name : "") +
                  (signature ? signature : "") + "\n";
  return reinterpret_cast<jmethodID>(method);
}

jint JNICALL get_version(JNIEnv*) { return JNI_VERSION_1_6; }
jclass JNICALL find_class(JNIEnv*, const char* name) {
  return class_handle(name);
}
void JNICALL delete_local_ref(JNIEnv*, jobject) {}
jclass JNICALL get_object_class(JNIEnv*, jobject object) {
  const auto* value = handle(object);
  return class_handle(value && value->kind == 1 ? "android/view/KeyEvent"
                                                : "java/lang/Object");
}
jmethodID JNICALL get_static_method_id(JNIEnv*, jclass, const char* name,
                                        const char* signature) {
  return method_handle(name, signature);
}
jmethodID JNICALL get_method_id(JNIEnv*, jclass, const char* name,
                                const char* signature) {
  return method_handle(name, signature);
}
jlong JNICALL call_static_long_method(JNIEnv*, jclass, jmethodID method, ...) {
  const auto* value = handle(method);
  if (value && value->value.starts_with("getProcessTimestamp")) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  return 0;
}
jlong JNICALL call_static_long_method_v(JNIEnv* env, jclass clazz,
                                        jmethodID method, va_list) {
  return call_static_long_method(env, clazz, method);
}
jlong JNICALL call_static_long_method_a(JNIEnv* env, jclass clazz,
                                        jmethodID method, const jvalue*) {
  return call_static_long_method(env, clazz, method);
}
jint JNICALL call_int_method(JNIEnv*, jobject object, jmethodID method, ...) {
  const auto* item = handle(object);
  const auto* name = handle(method);
  if (!item || !name) return 0;
  if (name->value.starts_with("getKeyCode")) return item->keycode;
  if (name->value.starts_with("getAction")) return item->action;
  if (name->value.starts_with("getRepeatCount")) return item->repeat;
  if (name->value.starts_with("getScanCode")) return item->scancode;
  if (name->value.starts_with("getMetaState")) return item->modifiers;
  return 0;
}
jint JNICALL call_int_method_v(JNIEnv* env, jobject object, jmethodID method,
                               va_list) {
  return call_int_method(env, object, method);
}
jint JNICALL call_int_method_a(JNIEnv* env, jobject object, jmethodID method,
                               const jvalue*) {
  return call_int_method(env, object, method);
}
jlong JNICALL call_long_method(JNIEnv*, jobject object, jmethodID method, ...) {
  const auto* item = handle(object);
  const auto* name = handle(method);
  return item && name && name->value.starts_with("getEventTime")
             ? static_cast<jlong>(item->event_time_ms)
             : 0;
}
jlong JNICALL call_long_method_v(JNIEnv* env, jobject object, jmethodID method,
                                 va_list) {
  return call_long_method(env, object, method);
}
jlong JNICALL call_long_method_a(JNIEnv* env, jobject object, jmethodID method,
                                 const jvalue*) {
  return call_long_method(env, object, method);
}
jboolean JNICALL exception_check(JNIEnv*) { return JNI_FALSE; }
void JNICALL exception_clear(JNIEnv*) {}
jint JNICALL get_java_vm(JNIEnv*, JavaVM** output) {
  if (!output || !active_runtime) return JNI_ERR;
  *output = nuah_jni_runtime_vm(active_runtime);
  return JNI_OK;
}
jint JNICALL register_natives(JNIEnv*, jclass clazz,
                              const JNINativeMethod* methods, jint count) {
  if (!clazz || !methods || count < 0) return JNI_ERR;
  const auto* class_name = handle(clazz);
  if (!class_name) return JNI_ERR;
  for (jint index = 0; index < count; ++index) {
    if (!methods[index].name || !methods[index].signature ||
        !methods[index].fnPtr ||
        nuah_jni_register_native(
            class_name->value.c_str(), methods[index].name,
            methods[index].signature,
            reinterpret_cast<NuahJniNativeFunction>(methods[index].fnPtr)) !=
            0) {
      return JNI_ERR;
    }
  }
  return JNI_OK;
}
jstring JNICALL new_string_utf(JNIEnv*, const char* value) {
  return reinterpret_cast<jstring>(object_handle(value));
}
const char* JNICALL get_string_utf_chars(JNIEnv*, jstring value,
                                          jboolean*) {
  const auto* item = handle(value);
  return item ? item->value.c_str() : "";
}
void JNICALL release_string_utf_chars(JNIEnv*, jstring, const char*) {}
jsize JNICALL get_string_utf_length(JNIEnv*, jstring value) {
  const auto* item = handle(value);
  return item ? static_cast<jsize>(item->value.size()) : 0;
}

jint JNICALL vm_get_env(JavaVM*, void** output, jint version) {
  if (!output || version != JNI_VERSION_1_6 || !active_runtime) return JNI_EVERSION;
  *output = nuah_jni_runtime_env(active_runtime);
  return JNI_OK;
}
jint JNICALL vm_attach(JavaVM*, void** output, void*) {
  if (!output || !active_runtime) return JNI_ERR;
  *output = nuah_jni_runtime_env(active_runtime);
  return JNI_OK;
}
jint JNICALL vm_detach(JavaVM*) { return JNI_OK; }
jint JNICALL vm_destroy(JavaVM*) { return JNI_ERR; }
}  // namespace

struct NuahJniRuntime {
  JNINativeInterface_ env_functions{};
  JNIInvokeInterface_ vm_functions{};
  JNIEnv env{&env_functions};
  JavaVM vm{&vm_functions};

  NuahJniRuntime() {
    env_functions.GetVersion = get_version;
    env_functions.FindClass = find_class;
    env_functions.DeleteLocalRef = delete_local_ref;
    env_functions.GetObjectClass = get_object_class;
    env_functions.GetStaticMethodID = get_static_method_id;
    env_functions.GetMethodID = get_method_id;
    env_functions.CallIntMethod = call_int_method;
    env_functions.CallIntMethodV = call_int_method_v;
    env_functions.CallIntMethodA = call_int_method_a;
    env_functions.CallLongMethod = call_long_method;
    env_functions.CallLongMethodV = call_long_method_v;
    env_functions.CallLongMethodA = call_long_method_a;
    env_functions.CallStaticLongMethod = call_static_long_method;
    env_functions.CallStaticLongMethodV = call_static_long_method_v;
    env_functions.CallStaticLongMethodA = call_static_long_method_a;
    env_functions.ExceptionCheck = exception_check;
    env_functions.ExceptionClear = exception_clear;
    env_functions.GetJavaVM = get_java_vm;
    env_functions.RegisterNatives = register_natives;
    env_functions.NewStringUTF = new_string_utf;
    env_functions.GetStringUTFChars = get_string_utf_chars;
    env_functions.ReleaseStringUTFChars = release_string_utf_chars;
    env_functions.GetStringUTFLength = get_string_utf_length;
    vm_functions.DestroyJavaVM = vm_destroy;
    vm_functions.AttachCurrentThread = vm_attach;
    vm_functions.DetachCurrentThread = vm_detach;
    vm_functions.GetEnv = vm_get_env;
    vm_functions.AttachCurrentThreadAsDaemon = vm_attach;
    active_runtime = this;
  }
  ~NuahJniRuntime() {
    if (active_runtime == this) active_runtime = nullptr;
  }
};

extern "C" NuahJniRuntime* nuah_jni_runtime_create(void) {
  return new NuahJniRuntime;
}
extern "C" void nuah_jni_runtime_destroy(NuahJniRuntime* runtime) {
  delete runtime;
}
extern "C" JavaVM* nuah_jni_runtime_vm(NuahJniRuntime* runtime) {
  return runtime ? &runtime->vm : nullptr;
}
extern "C" JNIEnv* nuah_jni_runtime_env(NuahJniRuntime* runtime) {
  return runtime ? &runtime->env : nullptr;
}

extern "C" int nuah_jni_runtime_dispatch_key(
    NuahJniRuntime* runtime, int keycode, int action, int repeat, int scancode,
    unsigned int modifiers, unsigned long long event_time_ms) {
  if (!runtime) return 0;
  constexpr const char* signature = "(JLandroid/view/KeyEvent;)Z";
  const char* method_name = action ? "onKeyDownNative" : "onKeyUpNative";
  const auto callback = nuah_jni_find_native_method(method_name, signature);
  if (!callback) {
    nuah_jni_report_missing("<registered>", method_name, signature);
    return 0;
  }
  auto* event = new Handle;
  event->kind = 1;
  event->keycode = keycode;
  event->action = action;
  event->repeat = repeat;
  event->scancode = scancode;
  event->modifiers = modifiers;
  event->event_time_ms = event_time_ms;
  using Callback = jboolean(JNICALL*)(JNIEnv*, jobject, jlong, jobject);
  const auto invoke = reinterpret_cast<Callback>(callback);
  const auto result = invoke(&runtime->env,
                             reinterpret_cast<jobject>(event), 0,
                             reinterpret_cast<jobject>(event));
  return result == JNI_TRUE ? 1 : 0;
}
