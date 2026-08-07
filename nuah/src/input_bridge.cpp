#include "nuah/input_bridge.h"
#include "nuah/native_session.h"

#include <SDL3/SDL.h>

#include <atomic>

namespace {
std::atomic<NuahInputSink> sink{nullptr};
std::atomic<void*> sink_data{nullptr};
std::atomic<bool> quit_requested{false};

void emit(const NuahInputEvent& event) {
  const auto callback = sink.load(std::memory_order_acquire);
  if (callback) callback(&event, sink_data.load(std::memory_order_acquire));
}
}

extern "C" int nuah_android_keycode_from_ascii(int ascii) {
  if (ascii == '0') return NUAH_KEY_0;
  if (ascii >= '1' && ascii <= '9') return NUAH_KEY_1 + (ascii - '1');
  if (ascii >= 'a' && ascii <= 'z') ascii -= 'a' - 'A';
  if (ascii >= 'A' && ascii <= 'Z') return NUAH_KEY_A + (ascii - 'A');
  return NUAH_KEY_UNKNOWN;
}

extern "C" int nuah_android_keycode_from_scancode(int scancode) {
  switch (scancode) {
    case SDL_SCANCODE_0: return NUAH_KEY_0;
    case SDL_SCANCODE_1: return NUAH_KEY_1;
    case SDL_SCANCODE_2: return NUAH_KEY_2;
    case SDL_SCANCODE_3: return NUAH_KEY_3;
    case SDL_SCANCODE_4: return NUAH_KEY_4;
    case SDL_SCANCODE_5: return NUAH_KEY_5;
    case SDL_SCANCODE_6: return NUAH_KEY_6;
    case SDL_SCANCODE_7: return NUAH_KEY_7;
    case SDL_SCANCODE_8: return NUAH_KEY_8;
    case SDL_SCANCODE_9: return NUAH_KEY_9;
    case SDL_SCANCODE_A: return NUAH_KEY_A;
    case SDL_SCANCODE_B: return NUAH_KEY_B;
    case SDL_SCANCODE_C: return NUAH_KEY_C;
    case SDL_SCANCODE_D: return NUAH_KEY_D;
    case SDL_SCANCODE_E: return NUAH_KEY_E;
    case SDL_SCANCODE_F: return NUAH_KEY_F;
    case SDL_SCANCODE_G: return NUAH_KEY_G;
    case SDL_SCANCODE_H: return NUAH_KEY_H;
    case SDL_SCANCODE_I: return NUAH_KEY_I;
    case SDL_SCANCODE_J: return NUAH_KEY_J;
    case SDL_SCANCODE_K: return NUAH_KEY_K;
    case SDL_SCANCODE_L: return NUAH_KEY_L;
    case SDL_SCANCODE_M: return NUAH_KEY_M;
    case SDL_SCANCODE_N: return NUAH_KEY_N;
    case SDL_SCANCODE_O: return NUAH_KEY_O;
    case SDL_SCANCODE_P: return NUAH_KEY_P;
    case SDL_SCANCODE_Q: return NUAH_KEY_Q;
    case SDL_SCANCODE_R: return NUAH_KEY_R;
    case SDL_SCANCODE_S: return NUAH_KEY_S;
    case SDL_SCANCODE_T: return NUAH_KEY_T;
    case SDL_SCANCODE_U: return NUAH_KEY_U;
    case SDL_SCANCODE_V: return NUAH_KEY_V;
    case SDL_SCANCODE_W: return NUAH_KEY_W;
    case SDL_SCANCODE_X: return NUAH_KEY_X;
    case SDL_SCANCODE_Y: return NUAH_KEY_Y;
    case SDL_SCANCODE_Z: return NUAH_KEY_Z;
    case SDL_SCANCODE_UP: return NUAH_KEY_DPAD_UP;
    case SDL_SCANCODE_DOWN: return NUAH_KEY_DPAD_DOWN;
    case SDL_SCANCODE_LEFT: return NUAH_KEY_DPAD_LEFT;
    case SDL_SCANCODE_RIGHT: return NUAH_KEY_DPAD_RIGHT;
    case SDL_SCANCODE_TAB: return NUAH_KEY_TAB;
    case SDL_SCANCODE_SPACE: return NUAH_KEY_SPACE;
    case SDL_SCANCODE_RETURN: return NUAH_KEY_ENTER;
    case SDL_SCANCODE_KP_ENTER: return NUAH_KEY_ENTER;
    case SDL_SCANCODE_BACKSPACE: return NUAH_KEY_DEL;
    case SDL_SCANCODE_ESCAPE: return NUAH_KEY_ESCAPE;
    case SDL_SCANCODE_LSHIFT: return NUAH_KEY_SHIFT_LEFT;
    case SDL_SCANCODE_RSHIFT: return NUAH_KEY_SHIFT_RIGHT;
    case SDL_SCANCODE_LCTRL: return NUAH_KEY_CTRL_LEFT;
    case SDL_SCANCODE_RCTRL: return NUAH_KEY_CTRL_RIGHT;
    case SDL_SCANCODE_LALT: return NUAH_KEY_ALT_LEFT;
    case SDL_SCANCODE_RALT: return NUAH_KEY_ALT_RIGHT;
    case SDL_SCANCODE_LGUI: return NUAH_KEY_META_LEFT;
    case SDL_SCANCODE_RGUI: return NUAH_KEY_META_RIGHT;
    case SDL_SCANCODE_CAPSLOCK: return NUAH_KEY_CAPS_LOCK;
    default: return NUAH_KEY_UNKNOWN;
  }
}

/* SDL and Android intentionally use different bit assignments for modifier
 * state.  Passing SDL_KMOD values through made Shift/Alt appear as unrelated
 * Android flags, so Java/Roblox could reject otherwise valid KeyEvents. */
unsigned int android_meta_state_from_sdl(SDL_Keymod modifiers) {
  unsigned int result = 0;
  if (modifiers & SDL_KMOD_SHIFT) result |= 0x1;       // META_SHIFT_ON
  if (modifiers & SDL_KMOD_LSHIFT) result |= 0x40;     // META_SHIFT_LEFT_ON
  if (modifiers & SDL_KMOD_RSHIFT) result |= 0x80;     // META_SHIFT_RIGHT_ON
  if (modifiers & SDL_KMOD_ALT) result |= 0x2;         // META_ALT_ON
  if (modifiers & SDL_KMOD_LALT) result |= 0x10;      // META_ALT_LEFT_ON
  if (modifiers & SDL_KMOD_RALT) result |= 0x20;      // META_ALT_RIGHT_ON
  if (modifiers & SDL_KMOD_CTRL) result |= 0x1000;     // META_CTRL_ON
  if (modifiers & SDL_KMOD_LCTRL) result |= 0x2000;   // META_CTRL_LEFT_ON
  if (modifiers & SDL_KMOD_RCTRL) result |= 0x4000;   // META_CTRL_RIGHT_ON
  if (modifiers & SDL_KMOD_GUI) result |= 0x10000;    // META_META_ON
  if (modifiers & SDL_KMOD_LGUI) result |= 0x20000;   // META_META_LEFT_ON
  if (modifiers & SDL_KMOD_RGUI) result |= 0x40000;   // META_META_RIGHT_ON
  if (modifiers & SDL_KMOD_CAPS) result |= 0x100000;  // META_CAPS_LOCK_ON
  if (modifiers & SDL_KMOD_NUM) result |= 0x200000;   // META_NUM_LOCK_ON
  if (modifiers & SDL_KMOD_SCROLL) result |= 0x400000; // META_SCROLL_LOCK_ON
  return result;
}

extern "C" void nuah_input_set_sink(NuahInputSink callback, void* user_data) {
  sink_data.store(user_data, std::memory_order_release);
  sink.store(callback, std::memory_order_release);
}

namespace {
void native_session_sink(const NuahInputEvent* event, void* user_data) {
  if (!event) return;
  auto* session = static_cast<NuahNativeSession*>(user_data);
  if (event->type == NUAH_INPUT_KEY) {
    nuah_native_session_dispatch_key(session, event->android_keycode,
                                     event->action, event->repeat,
                                     event->physical_scancode, event->modifiers,
                                     event->timestamp_ns / 1000000ULL);
  } else if (event->type == NUAH_INPUT_POINTER_MOTION ||
             event->type == NUAH_INPUT_POINTER_BUTTON ||
             event->type == NUAH_INPUT_POINTER_WHEEL) {
    nuah_native_session_dispatch_pointer(
        session, event->action, event->button, event->x, event->y, event->dx,
        event->dy, event->timestamp_ns / 1000000ULL);
  }
}
}

extern "C" void nuah_input_bind_native_session(NuahNativeSession* session) {
  nuah_input_set_sink(native_session_sink, session);
}

extern "C" int nuah_input_pump(void) {
  SDL_Event event{};
  int count = 0;
  while (SDL_PollEvent(&event)) {
    NuahInputEvent translated{};
    translated.timestamp_ns = event.common.timestamp;
    switch (event.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quit_requested.store(true, std::memory_order_release);
        break;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
        translated.type = NUAH_INPUT_KEY;
        translated.physical_scancode = static_cast<int>(event.key.scancode);
        translated.android_keycode = nuah_android_keycode_from_scancode(
            translated.physical_scancode);
        if (translated.android_keycode == NUAH_KEY_UNKNOWN) {
          translated.android_keycode =
              nuah_android_keycode_from_ascii(static_cast<int>(event.key.key));
        }
        translated.action = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0;
        translated.repeat = event.key.repeat ? 1 : 0;
        translated.modifiers = android_meta_state_from_sdl(event.key.mod);
        emit(translated);
        ++count;
        break;
      case SDL_EVENT_MOUSE_MOTION:
        translated.type = NUAH_INPUT_POINTER_MOTION;
        translated.action = 2;
        translated.x = event.motion.x;
        translated.y = event.motion.y;
        translated.dx = event.motion.xrel;
        translated.dy = event.motion.yrel;
        emit(translated);
        ++count;
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
        translated.type = NUAH_INPUT_POINTER_BUTTON;
        translated.action = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1 : 0;
        translated.button = static_cast<int>(event.button.button);
        translated.x = event.button.x;
        translated.y = event.button.y;
        emit(translated);
        ++count;
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        translated.type = NUAH_INPUT_POINTER_WHEEL;
        translated.action = 2;
        translated.dx = event.wheel.x;
        translated.dy = event.wheel.y;
        emit(translated);
        ++count;
        break;
      default:
        break;
    }
  }
  return count;
}

extern "C" int nuah_input_quit_requested(void) {
  return quit_requested.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" void nuah_input_reset_quit(void) {
  quit_requested.store(false, std::memory_order_release);
}
