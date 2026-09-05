#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum NuahAndroidKeyCode { NUAH_KEY_UNKNOWN = 0, NUAH_KEY_0 = 7, NUAH_KEY_1 = 8, NUAH_KEY_9 = 16, NUAH_KEY_DPAD_UP = 19, NUAH_KEY_DPAD_DOWN, NUAH_KEY_DPAD_LEFT, NUAH_KEY_DPAD_RIGHT,
  NUAH_KEY_A = 29, NUAH_KEY_W = 51, NUAH_KEY_Z = 54, NUAH_KEY_TAB = 61, NUAH_KEY_SPACE = 62, NUAH_KEY_ENTER = 66, NUAH_KEY_DEL = 67, NUAH_KEY_ESCAPE = 111,
  NUAH_KEY_CTRL_LEFT = 113, NUAH_KEY_CTRL_RIGHT = 114, NUAH_KEY_CAPS_LOCK = 115, NUAH_KEY_META_LEFT = 117, NUAH_KEY_META_RIGHT = 118, NUAH_KEY_SHIFT_LEFT = 59, NUAH_KEY_SHIFT_RIGHT = 60, NUAH_KEY_ALT_LEFT = 57, NUAH_KEY_ALT_RIGHT = 58, };
int nuah_android_keycode_from_ascii(int ascii);
/* Translate physical SDL scancodes for layout-independent movement keys. */
int nuah_android_keycode_from_scancode(int scancode);
/* Translate SDL's scancode enum to Android KeyEvent.getScanCode() values.
 * Android reports the raw evdev scan code for a physical keyboard; passing
 * SDL's enum here makes Roblox's hardware-key path see a different key. */
int nuah_android_scancode_from_sdl(int scancode);
enum NuahInputEventType { NUAH_INPUT_KEY = 1, NUAH_INPUT_POINTER_MOTION = 2, NUAH_INPUT_POINTER_BUTTON = 3, NUAH_INPUT_POINTER_WHEEL = 4, };
struct NuahInputEvent { int type, android_keycode, physical_scancode, action, repeat; /* 0=up, 1=down, 2=move */ unsigned int modifiers; unsigned long long timestamp_ns; double x, y, dx, dy; int button; };
typedef void (*NuahInputSink)(const NuahInputEvent* event, void* user_data);
typedef struct NuahNativeSession NuahNativeSession;
void nuah_input_set_sink(NuahInputSink sink, void* user_data); void nuah_input_bind_native_session(NuahNativeSession* session);
/* The Roblox surface draws its own pointer.  Hide/show the host compositor
 * cursor independently of relative-pointer capture. */
void nuah_input_set_host_cursor_hidden(int hidden); int nuah_input_quit_requested(void); void nuah_input_reset_quit(void); int nuah_input_pump(void);
#ifdef __cplusplus
}
#endif
