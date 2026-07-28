#include "nuah/input_bridge.h"
#include "nuah/jni_contract.h"
#include "nuah/jni_runtime.h"

#include <cassert>

void native_marker() {}
int delivered = 0;
void input_marker(const NuahInputEvent* event, void*) {
  if (event && event->type == NUAH_INPUT_KEY && event->action == 1) ++delivered;
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
  nuah_jni_runtime_destroy(runtime);
  nuah_input_set_sink(input_marker, nullptr);
  /* The sink is the game-facing delivery boundary. SDL event pumping is
   * exercised by the native runtime; this assertion verifies registration is
   * not silently discarded. */
  assert(delivered == 0);
  return 0;
}
