#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum NuahAndroidKeyCode {
  NUAH_KEY_UNKNOWN = 0,
  NUAH_KEY_0 = 7,
  NUAH_KEY_1 = 8, NUAH_KEY_2, NUAH_KEY_3, NUAH_KEY_4, NUAH_KEY_5,
  NUAH_KEY_6, NUAH_KEY_7, NUAH_KEY_8, NUAH_KEY_9,
  NUAH_KEY_DPAD_UP = 19, NUAH_KEY_DPAD_DOWN, NUAH_KEY_DPAD_LEFT,
  NUAH_KEY_DPAD_RIGHT,
  NUAH_KEY_VOLUME_UP = 24, NUAH_KEY_VOLUME_DOWN,
  NUAH_KEY_A = 29, NUAH_KEY_B, NUAH_KEY_C, NUAH_KEY_D, NUAH_KEY_E,
  NUAH_KEY_F, NUAH_KEY_G, NUAH_KEY_H, NUAH_KEY_I, NUAH_KEY_J,
  NUAH_KEY_K, NUAH_KEY_L, NUAH_KEY_M, NUAH_KEY_N, NUAH_KEY_O,
  NUAH_KEY_P, NUAH_KEY_Q, NUAH_KEY_R, NUAH_KEY_S, NUAH_KEY_T,
  NUAH_KEY_U, NUAH_KEY_V, NUAH_KEY_W, NUAH_KEY_X, NUAH_KEY_Y,
  NUAH_KEY_Z,
  NUAH_KEY_TAB = 61,
  NUAH_KEY_SPACE = 62,
  NUAH_KEY_ENTER = 66,
  NUAH_KEY_DEL = 67,
  NUAH_KEY_ESCAPE = 111,
  NUAH_KEY_CTRL_LEFT = 113,
  NUAH_KEY_CTRL_RIGHT = 114,
  NUAH_KEY_CAPS_LOCK = 115,
  NUAH_KEY_META_LEFT = 117,
  NUAH_KEY_META_RIGHT = 118,
  NUAH_KEY_SHIFT_LEFT = 59,
  NUAH_KEY_SHIFT_RIGHT = 60,
  NUAH_KEY_ALT_LEFT = 57,
  NUAH_KEY_ALT_RIGHT = 58,
};

int nuah_android_keycode_from_ascii(int ascii);
/* Translate physical SDL scancodes for layout-independent movement keys. */
int nuah_android_keycode_from_scancode(int scancode);
/* Translate SDL's scancode enum to Android KeyEvent.getScanCode() values.
 * Android reports the raw evdev scan code for a physical keyboard; passing
 * SDL's enum here makes Roblox's hardware-key path see a different key. */
int nuah_android_scancode_from_sdl(int scancode);

enum NuahInputEventType {
  NUAH_INPUT_KEY = 1,
  NUAH_INPUT_POINTER_MOTION = 2,
  NUAH_INPUT_POINTER_BUTTON = 3,
  NUAH_INPUT_POINTER_WHEEL = 4,
};

struct NuahInputEvent {
  int type;
  int android_keycode;
  int physical_scancode;
  int action; /* 0=up, 1=down, 2=move */
  int repeat;
  unsigned int modifiers;
  unsigned long long timestamp_ns;
  double x;
  double y;
  double dx;
  double dy;
  int button;
};

typedef void (*NuahInputSink)(const NuahInputEvent* event, void* user_data);
typedef struct NuahNativeSession NuahNativeSession;

void nuah_input_set_sink(NuahInputSink sink, void* user_data);
void nuah_input_bind_native_session(NuahNativeSession* session);
int nuah_input_quit_requested(void);
void nuah_input_reset_quit(void);
int nuah_input_pump(void);

#ifdef __cplusplus
}
#endif
