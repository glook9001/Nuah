#include "nuah/jni_runtime.h"
#include "nuah/jni_contract.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>

namespace {
struct Handle {
  std::string value;
};

thread_local NuahJniRuntime* active_runtime = nullptr;

Handle* handle(void* value) { return static_cast<Handle*>(value); }
jobject object_handle(const char* value) {
  return reinterpret_cast<jobject>(new Handle{value ? value : ""});
}
jclass class_handle(const char* value) {
  return reinterpret_cast<jclass>(new Handle{value ? value : ""});
}
jmethodID method_handle(const char* name, const char* signature) {
  return reinterpret_cast<jmethodID>(new Handle{
      std::string(name ? name : "") + signature + "\n"});
}

jint JNICALL get_version(JNIEnv*) { return JNI_VERSION_1_6; }
jclass JNICALL find_class(JNIEnv*, const char* name) {
  return class_handle(name);
}
void JNICALL delete_local_ref(JNIEnv*, jobject) {}
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
    env_functions.GetStaticMethodID = get_static_method_id;
    env_functions.GetMethodID = get_method_id;
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
