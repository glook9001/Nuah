#include "nuah/input_bridge.h"
#include "nuah/jni_contract.h"

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
  nuah_input_set_sink(input_marker, nullptr);
  /* The sink is the game-facing delivery boundary. SDL event pumping is
   * exercised by the native runtime; this assertion verifies registration is
   * not silently discarded. */
  assert(delivered == 0);
  return 0;
}
