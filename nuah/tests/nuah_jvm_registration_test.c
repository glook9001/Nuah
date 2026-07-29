#include "nuah/nuah_jvm.h"

#include "jvm/jni.h"

#include <stddef.h>

static void registered_callback(void) {}

int main(void) {
  NuahJvm* runtime = nuah_jvm_create();
  if (!runtime) return 1;

  JavaVM* vm = (JavaVM*)nuah_jvm_java_vm(runtime);
  if (!vm) {
    nuah_jvm_destroy(runtime);
    return 1;
  }
  JNIEnv* env = NULL;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) {
    nuah_jvm_destroy(runtime);
    return 1;
  }

  jclass activity = (*env)->FindClass(env, "com/google/androidgamesdk/GameActivity");
  if (!activity) {
    nuah_jvm_destroy(runtime);
    return 1;
  }
  union {
    void (*function)(void);
    void* pointer;
  } callback = {.function = registered_callback};
  const JNINativeMethod methods[] = {{"onKeyDownNative",
                                      "(JLandroid/view/KeyEvent;)Z",
                                      callback.pointer}};
  if ((*env)->RegisterNatives(env, activity, methods, 1) != JNI_OK) {
    nuah_jvm_destroy(runtime);
    return 1;
  }

  const int valid = nuah_jvm_find_registered_native(
                        runtime, "com/google/androidgamesdk/GameActivity",
                        "onKeyDownNative", "(JLandroid/view/KeyEvent;)Z") ==
                    callback.pointer;
  const int wrong_signature = nuah_jvm_find_registered_native(
                                  runtime,
                                  "com/google/androidgamesdk/GameActivity",
                                  "onKeyDownNative", "(J)Z") == NULL;
  const int wrong_method = nuah_jvm_find_registered_native(
                               runtime, "com/google/androidgamesdk/GameActivity",
                               "onKeyUpNative",
                               "(JLandroid/view/KeyEvent;)Z") == NULL;

  nuah_jvm_destroy(runtime);
  return valid && wrong_signature && wrong_method ? 0 : 1;
}
