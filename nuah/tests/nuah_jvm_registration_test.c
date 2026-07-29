#include "nuah/nuah_jvm.h"

#include "jvm/jni.h"

#include <assert.h>

static void registered_callback(void) {}

int main(void) {
  NuahJvm* runtime = nuah_jvm_create();
  assert(runtime);

  JavaVM* vm = (JavaVM*)nuah_jvm_java_vm(runtime);
  assert(vm);
  JNIEnv* env = NULL;
  assert((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK);
  assert(env);

  jclass activity = (*env)->FindClass(env, "com/google/androidgamesdk/GameActivity");
  assert(activity);
  union {
    void (*function)(void);
    void* pointer;
  } callback = {.function = registered_callback};
  const JNINativeMethod methods[] = {{"onKeyDownNative",
                                      "(JLandroid/view/KeyEvent;)Z",
                                      callback.pointer}};
  assert((*env)->RegisterNatives(env, activity, methods, 1) == JNI_OK);

  assert(nuah_jvm_find_registered_native(
             runtime, "com/google/androidgamesdk/GameActivity",
             "onKeyDownNative", "(JLandroid/view/KeyEvent;)Z") ==
         callback.pointer);
  assert(nuah_jvm_find_registered_native(
             runtime, "com/google/androidgamesdk/GameActivity",
             "onKeyDownNative", "(J)Z") == NULL);
  assert(nuah_jvm_find_registered_native(
             runtime, "com/google/androidgamesdk/GameActivity", "onKeyUpNative",
             "(JLandroid/view/KeyEvent;)Z") == NULL);

  nuah_jvm_destroy(runtime);
  return 0;
}
