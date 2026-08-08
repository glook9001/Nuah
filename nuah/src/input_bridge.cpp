#include "nuah/input_bridge.h"
#include "nuah/native_session.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<NuahInputSink> sink{nullptr};
std::atomic<void*> sink_data{nullptr};
std::atomic<bool> quit_requested{false};

/* Android's captured-pointer path does not report a new absolute pointer
 * position.  Sober's kl.e adapter keeps the last position and accumulates
 * MotionEvent AXIS_RELATIVE_X/Y into it before calling Roblox.  Keep the
 * same small state at the SDL boundary so MouseBehavior/MouseIcon sees the
 * same coordinates in a captured game window. */
double pointer_x = 0.0;
double pointer_y = 0.0;
bool pointer_position_valid = false;
bool relative_mouse_mode = false;

bool mouse_capture_enabled() {
  const char* value = std::getenv("NUAH_MOUSE_CAPTURE");
  return !value || std::strcmp(value, "0") != 0;
}

void set_relative_mouse_mode(SDL_Window* window, bool enabled) {
  if (!window) return;
  if (!SDL_SetWindowRelativeMouseMode(window, enabled)) {
    const char* trace = std::getenv("NUAH_INPUT_TRACE");
    if (trace && *trace)
      std::fprintf(stderr, "nuah input: relative mouse mode %s failed: %s\n",
                   enabled ? "on" : "off", SDL_GetError());
    return;
  }
  relative_mouse_mode = enabled;
  const char* trace = std::getenv("NUAH_INPUT_TRACE");
  if (trace && *trace)
    std::fprintf(stderr, "nuah input: relative mouse mode %s\n",
                 enabled ? "on" : "off");
}

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

extern "C" int nuah_android_scancode_from_sdl(int scancode) {
  /* Linux evdev values are the raw scan codes Android puts in KeyEvent.
   * Keep this table physical/layout-independent; the keyCode mapping above
   * remains responsible for the logical Android key. */
  switch (scancode) {
    case SDL_SCANCODE_ESCAPE: return 1;
    case SDL_SCANCODE_1: return 2;
    case SDL_SCANCODE_2: return 3;
    case SDL_SCANCODE_3: return 4;
    case SDL_SCANCODE_4: return 5;
    case SDL_SCANCODE_5: return 6;
    case SDL_SCANCODE_6: return 7;
    case SDL_SCANCODE_7: return 8;
    case SDL_SCANCODE_8: return 9;
    case SDL_SCANCODE_9: return 10;
    case SDL_SCANCODE_0: return 11;
    case SDL_SCANCODE_TAB: return 15;
    case SDL_SCANCODE_Q: return 16;
    case SDL_SCANCODE_W: return 17;
    case SDL_SCANCODE_E: return 18;
    case SDL_SCANCODE_R: return 19;
    case SDL_SCANCODE_T: return 20;
    case SDL_SCANCODE_Y: return 21;
    case SDL_SCANCODE_U: return 22;
    case SDL_SCANCODE_I: return 23;
    case SDL_SCANCODE_O: return 24;
    case SDL_SCANCODE_P: return 25;
    case SDL_SCANCODE_A: return 30;
    case SDL_SCANCODE_S: return 31;
    case SDL_SCANCODE_D: return 32;
    case SDL_SCANCODE_F: return 33;
    case SDL_SCANCODE_G: return 34;
    case SDL_SCANCODE_H: return 35;
    case SDL_SCANCODE_J: return 36;
    case SDL_SCANCODE_K: return 37;
    case SDL_SCANCODE_L: return 38;
    case SDL_SCANCODE_Z: return 44;
    case SDL_SCANCODE_X: return 45;
    case SDL_SCANCODE_C: return 46;
    case SDL_SCANCODE_V: return 47;
    case SDL_SCANCODE_B: return 48;
    case SDL_SCANCODE_N: return 49;
    case SDL_SCANCODE_M: return 50;
    case SDL_SCANCODE_LSHIFT: return 42;
    case SDL_SCANCODE_RSHIFT: return 54;
    case SDL_SCANCODE_LCTRL: return 29;
    case SDL_SCANCODE_RCTRL: return 97;
    case SDL_SCANCODE_LALT: return 56;
    case SDL_SCANCODE_RALT: return 100;
    case SDL_SCANCODE_SPACE: return 57;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return 28;
    case SDL_SCANCODE_BACKSPACE: return 14;
    case SDL_SCANCODE_UP: return 103;
    case SDL_SCANCODE_LEFT: return 105;
    case SDL_SCANCODE_RIGHT: return 106;
    case SDL_SCANCODE_DOWN: return 108;
    default:
      /* For keys outside the MVP, retaining SDL's value is more useful than
       * dropping the event; the gameplay keys above always use raw evdev. */
      return scancode;
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
    nuah_native_session_dispatch_pointer_event(
        session, event->type, event->action, event->button, event->x,
        event->y, event->dx, event->dy, event->timestamp_ns / 1000000ULL);
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
      {
        translated.type = NUAH_INPUT_KEY;
        const int sdl_scancode = static_cast<int>(event.key.scancode);
        translated.physical_scancode =
            nuah_android_scancode_from_sdl(sdl_scancode);
        translated.android_keycode =
            nuah_android_keycode_from_scancode(sdl_scancode);
        if (translated.android_keycode == NUAH_KEY_UNKNOWN) {
          translated.android_keycode =
              nuah_android_keycode_from_ascii(static_cast<int>(event.key.key));
        }
        translated.action = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0;
        translated.repeat = event.key.repeat ? 1 : 0;
        translated.modifiers = android_meta_state_from_sdl(event.key.mod);
        if (event.type == SDL_EVENT_KEY_DOWN &&
            translated.android_keycode == NUAH_KEY_ESCAPE) {
          set_relative_mouse_mode(SDL_GetWindowFromID(event.key.windowID),
                                  false);
        }
        emit(translated);
        ++count;
        break;
      }
      case SDL_EVENT_MOUSE_MOTION:
        translated.type = NUAH_INPUT_POINTER_MOTION;
        translated.action = 2;
        translated.dx = event.motion.xrel;
        translated.dy = event.motion.yrel;
        if (relative_mouse_mode) {
          /* SDL's relative-mode x/y may be the lock-center (often 0,0) on
           * Wayland.  Sober instead supplies the accumulated virtual
           * pointer position together with the relative delta. */
          if (!pointer_position_valid) {
            pointer_x = event.motion.x;
            pointer_y = event.motion.y;
            pointer_position_valid = true;
          } else {
            pointer_x += translated.dx;
            pointer_y += translated.dy;
          }
        } else {
          pointer_x = event.motion.x;
          pointer_y = event.motion.y;
          pointer_position_valid = true;
        }
        translated.x = pointer_x;
        translated.y = pointer_y;
        /* Wayland emits an initial cursor-position notification while the
         * SDL surface is being realized. Sober's Android adapter only calls
         * nativePassMouseMove for an actual motion/captured-pointer delta;
         * forwarding this (0,0) bootstrap event can reach Roblox before its
         * input state exists and corrupt its render startup. */
        if (translated.dx == 0.0 && translated.dy == 0.0) break;
        emit(translated);
        ++count;
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
        translated.type = NUAH_INPUT_POINTER_BUTTON;
        translated.action = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1 : 0;
        translated.button = static_cast<int>(event.button.button);
        pointer_x = event.button.x;
        pointer_y = event.button.y;
        pointer_position_valid = true;
        translated.x = event.button.x;
        translated.y = event.button.y;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            (translated.button == SDL_BUTTON_LEFT ||
             translated.button == SDL_BUTTON_RIGHT) &&
            mouse_capture_enabled()) {
          /* Sober's SurfaceView requests pointer capture when Roblox enters
           * mouse-lock mode. Nuah has no Android View hierarchy, so mirror
           * that contract at the SDL boundary: a game click or right-button
           * camera drag captures relative motion, and Escape releases it. */
          set_relative_mouse_mode(SDL_GetWindowFromID(event.button.windowID),
                                  true);
        }
        emit(translated);
        ++count;
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        translated.type = NUAH_INPUT_POINTER_WHEEL;
        translated.action = 2;
        if (!pointer_position_valid) {
          pointer_x = event.wheel.mouse_x;
          pointer_y = event.wheel.mouse_y;
          pointer_position_valid = true;
        }
        translated.x = pointer_x;
        translated.y = pointer_y;
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
