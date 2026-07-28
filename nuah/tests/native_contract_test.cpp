#include "nuah/input_bridge.h"
#include "nuah/jni_contract.h"
#include "nuah/jni_runtime.h"

#include <SDL3/SDL.h>

#include <cassert>

void native_marker() {}
int delivered = 0;
void input_marker(const NuahInputEvent* event, void*) {
  if (event && event->type == NUAH_INPUT_KEY && event->action == 1) ++delivered;
}
jboolean key_callback(JNIEnv* env, jobject, jlong, jobject event) {
  jclass clazz = env->GetObjectClass(event);
  jmethodID method = env->GetMethodID(clazz, "getKeyCode", "()I");
  return env->CallIntMethod(event, method) == NUAH_KEY_W ? JNI_TRUE : JNI_FALSE;
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
  auto* runtime = nuah_jni_runtime_create();
  if (!runtime) return 1;
  JavaVM* vm = nuah_jni_runtime_vm(runtime);
  JNIEnv* env = nuah_jni_runtime_env(runtime);
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
      env->RegisterNatives(logging, nullptr, 0) != JNI_ERR) return 1;
  constexpr const char* key_signature = "(JLandroid/view/KeyEvent;)Z";
  if (nuah_jni_register_native("com/google/androidgamesdk/GameActivity",
                               "onKeyDownNative", key_signature,
                               reinterpret_cast<NuahJniNativeFunction>(
                                   key_callback)) != 0 ||
      nuah_jni_runtime_dispatch_key(runtime, NUAH_KEY_W, 1, 0, 17, 0, 100) !=
          1) return 1;
  nuah_jni_runtime_destroy(runtime);
  nuah_input_set_sink(input_marker, nullptr);
  if (!SDL_Init(SDL_INIT_EVENTS)) return 1;
  SDL_Event key_event{};
  key_event.type = SDL_EVENT_KEY_DOWN;
  key_event.key.key = SDLK_W;
  key_event.key.scancode = SDL_SCANCODE_W;
  key_event.key.down = true;
  key_event.key.repeat = false;
  if (!SDL_PushEvent(&key_event) || nuah_input_pump() != 1 || delivered != 1) {
    SDL_Quit();
    return 1;
  }
  SDL_Quit();
  return 0;
}
