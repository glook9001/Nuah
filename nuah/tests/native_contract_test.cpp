#include "nuah/input_bridge.h"
#include "nuah/jni_contract.h"
#include "nuah/native_session.h"
#include "nuah/native_window_bridge.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <cassert>

void native_marker() {}
int key_callback_count = 0;
jboolean key_callback(JNIEnv* env, jobject, jlong, jobject event) {
  jclass clazz = env->GetObjectClass(event);
  jmethodID method = env->GetMethodID(clazz, "getKeyCode", "()I");
  if (env->CallIntMethod(event, method) != NUAH_KEY_W) return JNI_FALSE;
  ++key_callback_count;
  return JNI_TRUE;
}
jlong initialize_callback(JNIEnv*, jobject, jstring, jstring, jstring, jobject,
                          jbyteArray, jobject) {
  return 42;
}
int lifecycle_handle = 0;
void start_callback(JNIEnv*, jobject, jlong handle) {
  if (handle == 42) ++lifecycle_handle;
}
int surface_callback_count = 0;
void surface_created_callback(JNIEnv*, jobject, jlong handle, jobject surface) {
  if (handle == 42 && surface) ++surface_callback_count;
}
void surface_changed_callback(JNIEnv*, jobject, jlong handle, jobject surface,
                              jint, jint width, jint height) {
  if (handle == 42 && surface && width == 1280 && height == 720) {
    ++surface_callback_count;
  }
}
void surface_destroyed_callback(JNIEnv*, jobject, jlong handle, jobject surface) {
  if (handle == 42 && surface) ++surface_callback_count;
}

int main() {
  assert(nuah_android_keycode_from_ascii('w') == NUAH_KEY_W);
  assert(nuah_android_keycode_from_ascii('A') == NUAH_KEY_A);
  assert(nuah_android_keycode_from_ascii('9') == NUAH_KEY_9);
  assert(nuah_android_keycode_from_ascii('0') == NUAH_KEY_UNKNOWN);
  assert(nuah_jni_register_native("com/roblox/Game", "nativeStart", "()V",
                                  native_marker) == 0);
  assert(nuah_jni_find_native("com/roblox/Game", "nativeStart", "()V") ==
         native_marker);
  assert(nuah_jni_registered_count() == 1);
  auto* runtime = nuah_native_session_create();
  if (!runtime) return 1;
  auto* jvm = nuah_native_session_jvm(runtime);
  JavaVM* vm = reinterpret_cast<JavaVM*>(nuah_jvm_java_vm(jvm));
  JNIEnv* env = reinterpret_cast<JNIEnv*>(nuah_jvm_jni_env(jvm));
  if (!vm || !env) return 1;
  void* attached = nullptr;
  if (vm->GetEnv(&attached, JNI_VERSION_1_6) != JNI_OK || attached != env ||
      env->GetVersion() != JNI_VERSION_1_6) return 1;
  jclass logging = env->FindClass(
      "com/roblox/universalapp/logging/LoggingProtocol");
  jmethodID timestamp = env->GetStaticMethodID(logging,
                                                "getProcessTimestamp", "()J");
  if (!timestamp || env->CallStaticLongMethod(logging, timestamp) <= 0 ||
      env->ExceptionCheck() != JNI_FALSE ||
      env->RegisterNatives(logging, nullptr, 0) != JNI_OK) return 1;
  constexpr const char* key_signature = "(JLandroid/view/KeyEvent;)Z";
  const auto activity = env->FindClass("com/google/androidgamesdk/GameActivity");
  const JNINativeMethod methods[] = {
      {const_cast<char*>("onKeyDownNative"), const_cast<char*>(key_signature),
       reinterpret_cast<void*>(key_callback)},
      {const_cast<char*>("initializeNativeCode"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                         "Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J"),
       reinterpret_cast<void*>(initialize_callback)},
      {const_cast<char*>("onStartNative"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(start_callback)},
      {const_cast<char*>("onSurfaceCreatedNative"),
       const_cast<char*>("(JLandroid/view/Surface;)V"),
       reinterpret_cast<void*>(surface_created_callback)},
      {const_cast<char*>("onSurfaceChangedNative"),
       const_cast<char*>("(JLandroid/view/Surface;III)V"),
       reinterpret_cast<void*>(surface_changed_callback)},
      {const_cast<char*>("onSurfaceDestroyedNative"),
       const_cast<char*>("(JLandroid/view/Surface;)V"),
       reinterpret_cast<void*>(surface_destroyed_callback)},
  };
  if (!activity || env->RegisterNatives(activity, methods, 6) != JNI_OK ||
      nuah_native_session_dispatch_key(runtime, NUAH_KEY_W, 1, 0, 17, 0, 100) != 1 ||
      nuah_native_session_initialize_game(runtime, "com.roblox.client", "/tmp") != 42 ||
      nuah_native_session_dispatch_lifecycle(runtime, "onStartNative") != 1 ||
      lifecycle_handle != 1) return 1;
  int native_surface_key = 0;
  auto* native_window = nuah_native_window_register_surface(
      &native_surface_key, &native_surface_key, 1280, 720);
  void* surface = nuah_native_session_surface(runtime, native_window);
  auto* resolved_window = nuah_native_window_from_surface(surface);
  if (!native_window || !surface || resolved_window != native_window ||
      !nuah_native_session_dispatch_surface_created(runtime, surface) ||
      !nuah_native_session_dispatch_surface_changed(runtime, surface, 0, 1280, 720) ||
      !nuah_native_session_dispatch_surface_destroyed(runtime, surface) ||
      surface_callback_count != 3) {
    if (resolved_window) nuah_native_window_release(resolved_window);
    if (native_window) nuah_native_window_unregister_surface(&native_surface_key);
    return 1;
  }
  nuah_native_window_release(resolved_window);
  nuah_native_session_clear_surface(runtime);
  nuah_native_window_unregister_surface(&native_surface_key);
  nuah_input_bind_native_session(runtime);
  if (!SDL_Init(SDL_INIT_EVENTS)) return 1;
  SDL_Event key_event{};
  key_event.type = SDL_EVENT_KEY_DOWN;
  key_event.key.key = SDLK_W;
  key_event.key.scancode = SDL_SCANCODE_W;
  key_event.key.down = true;
  key_event.key.repeat = false;
  if (!SDL_PushEvent(&key_event) || nuah_input_pump() != 1 ||
      key_callback_count != 2) {
    SDL_Quit();
    return 1;
  }
  SDL_Quit();
  nuah_native_session_destroy(runtime);
  return 0;
}
