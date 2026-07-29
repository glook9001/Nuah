#include "jni_facade.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef jint (*JniOnLoad)(JavaVM*, void*);

static struct JNINativeInterface env_table;
static JNIEnv env = &env_table;
static struct JNIInvokeInterface vm_table;
static JavaVM vm = &vm_table;
static int registered_native_count;
static char class_token;
static char method_token;

static jint JNICALL facade_get_version(JNIEnv* ignored_env) {
  (void)ignored_env;
  return JNI_VERSION_1_6;
}
static jclass JNICALL facade_find_class(JNIEnv* ignored_env, const char* ignored_name) {
  (void)ignored_env;
  (void)ignored_name;
  return (jclass)&class_token;
}
static jclass JNICALL facade_get_object_class(JNIEnv* ignored_env, jobject ignored_object) {
  (void)ignored_env;
  (void)ignored_object;
  return (jclass)&class_token;
}
static jmethodID JNICALL facade_get_method_id(JNIEnv* ignored_env, jclass ignored_class,
                                               const char* ignored_name,
                                               const char* ignored_signature) {
  (void)ignored_env;
  (void)ignored_class;
  (void)ignored_name;
  (void)ignored_signature;
  return (jmethodID)&method_token;
}
static jmethodID JNICALL facade_get_static_method_id(JNIEnv* ignored_env,
                                                      jclass ignored_class,
                                                      const char* ignored_name,
                                                      const char* ignored_signature) {
  (void)ignored_env;
  (void)ignored_class;
  (void)ignored_name;
  (void)ignored_signature;
  return (jmethodID)&method_token;
}
static jint JNICALL facade_register_natives(JNIEnv* ignored_env, jclass ignored_class,
                                             const JNINativeMethod* methods,
                                             jint count) {
  (void)ignored_env;
  (void)ignored_class;
  if (!methods || count < 0) return JNI_ERR;
  registered_native_count += count;
  return JNI_OK;
}
static jint JNICALL facade_get_java_vm(JNIEnv* ignored_env, JavaVM** output) {
  (void)ignored_env;
  if (!output) return JNI_ERR;
  *output = &vm;
  return JNI_OK;
}
static jboolean JNICALL facade_exception_check(JNIEnv* ignored_env) {
  (void)ignored_env;
  return JNI_FALSE;
}
static void JNICALL facade_exception_clear(JNIEnv* ignored_env) { (void)ignored_env; }
static void JNICALL facade_delete_local_ref(JNIEnv* ignored_env, jobject ignored_object) {
  (void)ignored_env;
  (void)ignored_object;
}
static jobject JNICALL facade_new_global_ref(JNIEnv* ignored_env, jobject object) {
  (void)ignored_env;
  return object;
}
static void JNICALL facade_delete_global_ref(JNIEnv* ignored_env, jobject ignored_object) {
  (void)ignored_env;
  (void)ignored_object;
}
static jobject JNICALL facade_new_local_ref(JNIEnv* ignored_env, jobject object) {
  (void)ignored_env;
  return object;
}
static jstring JNICALL facade_new_string_utf(JNIEnv* ignored_env, const char* value) {
  (void)ignored_env;
  return (jstring)(value ? value : "");
}
static const char* JNICALL facade_get_string_utf_chars(JNIEnv* ignored_env, jstring value,
                                                        jboolean* ignored_copy) {
  (void)ignored_env;
  (void)ignored_copy;
  return value ? (const char*)value : "";
}
static void JNICALL facade_release_string_utf_chars(JNIEnv* ignored_env,
                                                    jstring ignored_value,
                                                    const char* ignored_chars) {
  (void)ignored_env;
  (void)ignored_value;
  (void)ignored_chars;
}
static jsize JNICALL facade_get_string_utf_length(JNIEnv* ignored_env, jstring value) {
  (void)ignored_env;
  return value ? (jsize)strlen((const char*)value) : 0;
}
static jint JNICALL facade_vm_get_env(JavaVM* ignored_vm, void** output, jint version) {
  (void)ignored_vm;
  if (!output || version != JNI_VERSION_1_6) return JNI_EVERSION;
  *output = &env;
  return JNI_OK;
}
static jint JNICALL facade_vm_attach(JavaVM* ignored_vm, JNIEnv** output,
                                     void* ignored_args) {
  (void)ignored_vm;
  (void)ignored_args;
  if (!output) return JNI_ERR;
  *output = &env;
  return JNI_OK;
}
static jint JNICALL facade_vm_detach(JavaVM* ignored_vm) {
  (void)ignored_vm;
  return JNI_OK;
}
static jint JNICALL facade_vm_destroy(JavaVM* ignored_vm) {
  (void)ignored_vm;
  return JNI_ERR;
}

static void configure_facade(void) {
  memset(&env_table, 0, sizeof(env_table));
  memset(&vm_table, 0, sizeof(vm_table));
  env_table.GetVersion = facade_get_version;
  env_table.FindClass = facade_find_class;
  env_table.GetObjectClass = facade_get_object_class;
  env_table.GetMethodID = facade_get_method_id;
  env_table.GetStaticMethodID = facade_get_static_method_id;
  env_table.RegisterNatives = facade_register_natives;
  env_table.GetJavaVM = facade_get_java_vm;
  env_table.ExceptionCheck = facade_exception_check;
  env_table.ExceptionClear = facade_exception_clear;
  env_table.DeleteLocalRef = facade_delete_local_ref;
  env_table.NewGlobalRef = facade_new_global_ref;
  env_table.DeleteGlobalRef = facade_delete_global_ref;
  env_table.NewLocalRef = facade_new_local_ref;
  env_table.NewStringUTF = facade_new_string_utf;
  env_table.GetStringUTFChars = facade_get_string_utf_chars;
  env_table.ReleaseStringUTFChars = facade_release_string_utf_chars;
  env_table.GetStringUTFLength = facade_get_string_utf_length;
  vm_table.DestroyJavaVM = facade_vm_destroy;
  vm_table.AttachCurrentThread = facade_vm_attach;
  vm_table.DetachCurrentThread = facade_vm_detach;
  vm_table.GetEnv = facade_vm_get_env;
  vm_table.AttachCurrentThreadAsDaemon = facade_vm_attach;
}

int nuah_jni_invoke_onload(void* module) {
  const JniOnLoad onload = (JniOnLoad)dlsym(module, "JNI_OnLoad");
  if (!onload) {
    fprintf(stderr, "bionic JNI error: libroblox.so has no JNI_OnLoad\n");
    return 3;
  }
  registered_native_count = 0;
  configure_facade();
  fprintf(stderr, "bionic JNI_OnLoad entry=%p\n", (void*)onload);
  const jint version = onload(&vm, 0);
  if (version != JNI_VERSION_1_6 && version != JNI_VERSION_1_4 &&
      version != JNI_VERSION_1_2) {
    fprintf(stderr, "bionic JNI error: JNI_OnLoad returned unsupported version 0x%x\n",
            (unsigned int)version);
    return 4;
  }
  fprintf(stderr, "bionic JNI_OnLoad accepted version 0x%x; registered natives=%d\n",
          (unsigned int)version, registered_native_count);
  return 0;
}
