#include "nuah/input_bridge.h"
#include "nuah/native_session.h"
#include "nuah/perf_metrics.h"

#include <SDL3/SDL.h>

#include <algorithm>
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
/* SDL/Wayland may enqueue one absolute motion sample when a relative-pointer
 * constraint is released.  It is a compositor transition, not user input;
 * forwarding it would overwrite Roblox's captured coordinates (often with
 * 0,0) immediately after Escape. */
bool suppress_next_absolute_motion = false;
bool host_window_focused = false;
/* Android focus is a state, not an event stream.  Wayland can report a short
 * compositor focus blip while the surface is resized or Roblox changes its
 * input mode; forwarding that blip as Activity false/true leaves the native
 * input queue in the same stuck state that Alt-Tab happens to repair. */
bool android_focus_sent = false;
SDL_Cursor* invisible_cursor = nullptr;
/* SDL may not deliver a button-up after a compositor focus transition.  Keep
 * the host-side button state so we can synthesize releases instead of leaving
 * Roblox's NativeInputInterface believing that fire is still held. */
unsigned int buttons_down = 0;
bool focus_loss_pending = false;
SDL_Window* focus_loss_window = nullptr;
Uint64 focus_loss_deadline_ns = 0;

/* Keyboard/pointer FOCUS_LOST is not an Android activity pause.  KDE and
 * zwp_pointer_constraints emit that SDL event for seconds while the toplevel
 * stays mapped; forwarding it as onWindowFocusChanged(false) pauses Roblox
 * and then re-acquiring relative mouse restarts the same cycle.  Only a
 * hidden or minimized host window is a real activity loss. */
bool window_mapped_for_android(SDL_Window* window) {
  if (!window) return false;
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
}

bool mouse_capture_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("NUAH_MOUSE_CAPTURE");
    /* Roblox's desktop adapter uses Android pointer capture whenever its own
     * mouse-lock callback says the camera is centered.  Keep that contract by
     * default; NUAH_MOUSE_CAPTURE=0 remains a diagnostic escape hatch for
     * compositors without relative-pointer support. */
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

bool input_trace_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("NUAH_INPUT_TRACE");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

bool set_relative_mouse_mode(SDL_Window* window, bool enabled);
void release_pressed_buttons(SDL_Window* window,
                             unsigned long long timestamp);

SDL_Cursor* get_invisible_cursor() {
  if (invisible_cursor) return invisible_cursor;
  /* SDL's monochrome cursor format uses a zero mask for transparent pixels.
   * Keep this cursor process-local; it is only installed while Roblox owns
   * the pointer through mouse-lock. */
  static const Uint8 pixels[8] = {};
  static const Uint8 mask[8] = {};
  invisible_cursor = SDL_CreateCursor(pixels, mask, 8, 8, 0, 0);
  return invisible_cursor;
}

void set_host_cursor_hidden(bool hidden) {
  if (hidden) {
    if (SDL_Cursor* cursor = get_invisible_cursor())
      (void)SDL_SetCursor(cursor);
    (void)SDL_HideCursor();
  } else {
    (void)SDL_SetCursor(SDL_GetDefaultCursor());
    (void)SDL_ShowCursor();
  }
}

NuahNativeSession* bound_native_session() {
  return static_cast<NuahNativeSession*>(
      sink_data.load(std::memory_order_acquire));
}

/* Match Sober's kl.e adapter: persistent capture follows Roblox's
 * nativeGetMainWindowIsMouseLockedCenter() result.  The View requests pointer
 * capture from its generic-motion path; button edges do not change it. */
bool sync_mouse_lock(SDL_Window* window, NuahNativeSession* session,
                     bool discard_on_lock) {
  if (!window || !session) return false;
  if (!mouse_capture_enabled()) {
    if (relative_mouse_mode) set_relative_mouse_mode(window, false);
    return false;
  }
  const int locked = nuah_native_session_is_mouse_locked_center(session);
  if (locked < 0) return false;
  const bool should_capture = locked != 0;
  if (should_capture == relative_mouse_mode) return false;
  /* When Roblox leaves mouse-lock (death, a menu, or a surface rebuild),
   * release anything held in the old input epoch before removing the SDL
   * constraint.  Otherwise a lost Android button-up can keep firing disabled
   * until an external focus cycle.  Do not release on acquisition: a held
   * left/right button must remain held while the camera enters lock mode. */
  if (!should_capture && buttons_down)
    release_pressed_buttons(window, SDL_GetTicksNS());
  const bool capture_changed = set_relative_mouse_mode(window, should_capture);
  /* Android's generic-motion path requests capture and drops that one event;
   * the following captured event is the first one sent to Roblox. */
  return discard_on_lock && should_capture && capture_changed;
}

bool set_relative_mouse_mode(SDL_Window* window, bool enabled) {
  if (!window) return false;
  if (relative_mouse_mode == enabled &&
      SDL_GetWindowRelativeMouseMode(window) == enabled)
    return true;
  if (enabled) {
    suppress_next_absolute_motion = false;
    /* Do not teleport the logical pointer to an artificial centre when
     * Roblox asks for capture.  Sober keeps the last pointer
     * position and then accumulates captured relative deltas.  Preserve the
     * last SDL position when available; only seed it from SDL when this is
     * the first event in the window. */
    if (!pointer_position_valid) {
      float mouse_x = 0.0f;
      float mouse_y = 0.0f;
      (void)SDL_GetMouseState(&mouse_x, &mouse_y);
      pointer_x = mouse_x;
      pointer_y = mouse_y;
      pointer_position_valid = true;
    }
  }
  if (!enabled && relative_mouse_mode)
    suppress_next_absolute_motion = true;
  const bool relative_ok = SDL_SetWindowRelativeMouseMode(window, enabled);
  /* SDL may complete a Wayland pointer-constraint transition asynchronously;
   * use SDL's reported state rather than assuming a failed request changed
   * nothing.  This prevents the next lock query from dropping every motion
   * event while the modes disagree. */
  relative_mouse_mode = SDL_GetWindowRelativeMouseMode(window);
  if (!relative_ok) {
    if (input_trace_enabled())
      std::fprintf(stderr, "nuah input: relative mouse mode %s failed: %s\n",
                   enabled ? "on" : "off", SDL_GetError());
    /* Hiding the host cursor is independent of relative-pointer support.
     * Do not add a mouse grab fallback here: on Wayland it can steal focus and
     * is the source of intermittent button/capture transitions. */
  }
  if (input_trace_enabled())
    std::fprintf(stderr, "nuah input: relative mouse mode %s\n",
                 enabled ? "on" : "off");
  return relative_mouse_mode == enabled;
}

void emit(const NuahInputEvent& event) {
  const auto callback = sink.load(std::memory_order_acquire);
  if (callback) callback(&event, sink_data.load(std::memory_order_acquire));
}

unsigned int button_bit(int button) {
  return button >= 1 && button <= 31 ? (1u << (button - 1)) : 0u;
}

/* The host window may be maximized while the Android surface stays at its
 * launch size (NUAH_LOCK_SURFACE_SIZE).  SDL reports host-window coordinates,
 * but Roblox hit-tests against the Android surface.  Keep the host-space
 * virtual pointer internally and translate only at the boundary. */
bool locked_surface_dimensions(int* width, int* height) {
  if (!width || !height) return false;
  static const struct LockedDimensions {
    bool valid = false;
    int w = 0;
    int h = 0;
  } dims = [] {
    LockedDimensions res;
    const char* locked = std::getenv("NUAH_LOCK_SURFACE_SIZE");
    if (!locked || !*locked || std::strcmp(locked, "0") == 0) return res;
    const char* width_value = std::getenv("NUAH_SURFACE_WIDTH");
    const char* height_value = std::getenv("NUAH_SURFACE_HEIGHT");
    if (!width_value || !height_value) return res;
    char* width_end = nullptr;
    char* height_end = nullptr;
    const long parsed_width = std::strtol(width_value, &width_end, 10);
    const long parsed_height = std::strtol(height_value, &height_end, 10);
    if (width_end == width_value || *width_end != '\0' ||
        height_end == height_value || *height_end != '\0' ||
        parsed_width <= 0 || parsed_height <= 0)
      return res;
    res.valid = true;
    res.w = static_cast<int>(parsed_width);
    res.h = static_cast<int>(parsed_height);
    return res;
  }();
  if (!dims.valid) return false;
  *width = dims.w;
  *height = dims.h;
  return true;
}

void map_host_delta_to_surface(SDL_Window* window, double* dx, double* dy) {
  if (!dx || !dy) return;
  int surface_width = 0;
  int surface_height = 0;
  int host_width = 0;
  int host_height = 0;
  if (!locked_surface_dimensions(&surface_width, &surface_height) ||
      !window || SDL_GetWindowSize(window, &host_width, &host_height) <= 0 ||
      host_width <= 0 || host_height <= 0)
    return;
  *dx *= static_cast<double>(surface_width) /
         static_cast<double>(host_width);
  *dy *= static_cast<double>(surface_height) /
         static_cast<double>(host_height);
}

/* Relative camera coordinates may legitimately accumulate past the view
 * edges.  Roblox still expects discrete button/wheel coordinates inside the
 * Android surface, however.  Normalize only those discrete events; leave
 * pointer_x/pointer_y untouched so relative camera motion remains continuous. */
void surface_pointer_position(SDL_Window* window, double* x, double* y) {
  if (!x || !y) return;
  int width = 0;
  int height = 0;
  if (!window || SDL_GetWindowSize(window, &width, &height) <= 0 ||
      width <= 0 || height <= 0)
    return;
  *x = std::clamp(*x, 0.0, static_cast<double>(width - 1));
  *y = std::clamp(*y, 0.0, static_cast<double>(height - 1));
  int surface_width = 0;
  int surface_height = 0;
  if (!locked_surface_dimensions(&surface_width, &surface_height)) return;
  *x = std::clamp(*x * static_cast<double>(surface_width) /
                      static_cast<double>(width),
                  0.0, static_cast<double>(surface_width - 1));
  *y = std::clamp(*y * static_cast<double>(surface_height) /
                      static_cast<double>(height),
                  0.0, static_cast<double>(surface_height - 1));
}

void release_pressed_buttons(SDL_Window* window, unsigned long long timestamp) {
  double event_x = pointer_x;
  double event_y = pointer_y;
  surface_pointer_position(window, &event_x, &event_y);
  for (int button = 1; button <= 5; ++button) {
    const unsigned int bit = button_bit(button);
    if (!(buttons_down & bit)) continue;
    NuahInputEvent release{};
    release.type = NUAH_INPUT_POINTER_BUTTON;
    release.action = 0;
    release.button = button;
    release.timestamp_ns = timestamp;
    release.x = event_x;
    release.y = event_y;
    emit(release);
    buttons_down &= ~bit;
  }
  /* A focus loss also ends Android pointer capture.  Roblox's next locked
   * motion will request it again through the normal generic-motion path. */
  (void)set_relative_mouse_mode(window, false);
}

/* A Wayland pointer-constraint transition can briefly lose the SDL focus
 * event that normally brackets a button release.  Once the compositor has
 * settled, reconcile SDL's physical button mask with the events already sent
 * to Roblox.  This is intentionally limited to the five standard buttons;
 * wheel and motion continue through their normal SDL events. */
void reconcile_mouse_buttons(SDL_Window* window, unsigned long long timestamp) {
  if (!window || !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) return;
  float host_x = 0.0f;
  float host_y = 0.0f;
  const SDL_MouseButtonFlags host_buttons =
      SDL_GetMouseState(&host_x, &host_y);
  if (!relative_mouse_mode && !pointer_position_valid) {
    pointer_x = host_x;
    pointer_y = host_y;
    pointer_position_valid = true;
  }
  for (int button = 1; button <= 5; ++button) {
    const unsigned int bit = button_bit(button);
    const bool host_down = (host_buttons & SDL_BUTTON_MASK(button)) != 0;
    const bool sent_down = (buttons_down & bit) != 0;
    if (host_down == sent_down) continue;
    NuahInputEvent event{};
    event.type = NUAH_INPUT_POINTER_BUTTON;
    event.action = host_down ? 1 : 0;
    event.button = button;
    event.timestamp_ns = timestamp;
    event.x = pointer_x;
    event.y = pointer_y;
    emit(event);
    if (host_down)
      buttons_down |= bit;
    else
      buttons_down &= ~bit;
  }
}

void service_focus_loss() {
  if (!focus_loss_pending || !focus_loss_window) return;
  if (window_mapped_for_android(focus_loss_window)) {
    focus_loss_pending = false;
    focus_loss_window = nullptr;
    return;
  }
  if (SDL_GetTicksNS() < focus_loss_deadline_ns) return;
  release_pressed_buttons(focus_loss_window, SDL_GetTicksNS());
  if (android_focus_sent) {
    if (NuahNativeSession* session = bound_native_session())
      (void)nuah_native_session_dispatch_window_focus(session, 0);
    android_focus_sent = false;
  }
  focus_loss_pending = false;
  focus_loss_window = nullptr;
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
  /* native_runtime has already delivered the initial Android focus callback
   * before binding the SDL pump.  Seed the debounce state from SDL so the
   * first queued focus event does not replay a duplicate true/false pair. */
  host_window_focused = SDL_GetKeyboardFocus() != nullptr;
  android_focus_sent = host_window_focused;
}

extern "C" void nuah_input_set_host_cursor_hidden(int hidden) {
  set_host_cursor_hidden(hidden != 0);
}

extern "C" int nuah_input_pump(void) {
  const Uint64 pump_start = nuah_perf_trace_enabled() ? SDL_GetTicksNS() : 0;
  service_focus_loss();
  SDL_Event event{};
  int count = 0;
  static const bool nonblocking_events = [] {
    const char* value = std::getenv("NUAH_NONBLOCK_WAYLAND_EVENTS");
    /* Keep this in lock-step with window_session's Wayland pump.  The
     * non-blocking path starves the shared display round-trip on the Intel
     * host; the observed result is severe frame pacing loss. */
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  /* Wayland can deliver hundreds of relative-pointer samples between two
   * 10-ms host pumps. Android's MotionEvent path is allowed to coalesce
   * history, so forward one event with the accumulated delta instead of
   * entering Roblox/JNI once per compositor sample. Disable this only when
   * comparing raw input timing. */
  static const bool coalesce_motion = [] {
    const char* value = std::getenv("NUAH_INPUT_COALESCE");
    return !value || std::strcmp(value, "0") != 0;
  }();
  /* Diagnostic A/B only: retain SDL motion processing and Roblox mouse-lock
   * synchronization, but do not forward camera deltas into the APK.  This
   * isolates Roblox's motion-driven camera/render work from Nuah's input
   * bridge.  Buttons and wheel events remain live so the test can be stopped
   * without leaving a pressed-button state behind. */
  static const bool drop_mouse_motion = [] {
    const char* value = std::getenv("NUAH_DROP_MOUSE_MOTION");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  NuahInputEvent pending_motion{};
  bool has_pending_motion = false;
  bool motion_lock_synced = false;
  auto flush_motion = [&] {
    if (!has_pending_motion) return;
    emit(pending_motion);
    ++count;
    pending_motion = {};
    has_pending_motion = false;
  };
  auto next_event = [&] {
    if (!nonblocking_events) return SDL_PollEvent(&event);
    return SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                          SDL_EVENT_LAST) > 0;
  };
  while (next_event()) {
    if (event.type != SDL_EVENT_MOUSE_MOTION) {
      /* Preserve SDL ordering: a key/button/focus event must observe all
       * preceding motion, and a later motion must re-check Roblox's lock
       * state after that event has been delivered. */
      flush_motion();
      motion_lock_synced = false;
    }
    NuahInputEvent translated{};
    translated.timestamp_ns = event.common.timestamp;
    switch (event.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        std::fprintf(stderr, "nuah input: %s window=%u\n",
                     event.type == SDL_EVENT_QUIT ? "SDL_EVENT_QUIT"
                                                  : "window close requested",
                     event.window.windowID);
        release_pressed_buttons(
            SDL_GetWindowFromID(event.window.windowID), event.common.timestamp);
        quit_requested.store(true, std::memory_order_release);
        break;
      case SDL_EVENT_WINDOW_HIDDEN:
      case SDL_EVENT_WINDOW_MINIMIZED: {
        SDL_Window* window = SDL_GetWindowFromID(event.window.windowID);
        host_window_focused = false;
        focus_loss_pending = true;
        focus_loss_window = window;
        focus_loss_deadline_ns = SDL_GetTicksNS();
        if (input_trace_enabled())
          std::fprintf(stderr, "nuah input: host window unmapped\n");
        break;
      }
      case SDL_EVENT_WINDOW_FOCUS_LOST: {
        host_window_focused = false;
        SDL_Window* window = SDL_GetWindowFromID(event.window.windowID);
        /* Keep relative-mouse and the Android-hidden cursor.  A mapped
         * toplevel that lost keyboard focus is still the game surface. */
        if (!window_mapped_for_android(window)) {
          focus_loss_pending = true;
          focus_loss_window = window;
          focus_loss_deadline_ns = SDL_GetTicksNS();
        }
        break;
      }
      case SDL_EVENT_WINDOW_SHOWN:
      case SDL_EVENT_WINDOW_RESTORED:
      case SDL_EVENT_WINDOW_FOCUS_GAINED: {
        host_window_focused = true;
        /* Roblox's Android surface owns the visible cursor; the desktop
         * cursor must not be composited on top of it. */
        set_host_cursor_hidden(true);
        focus_loss_pending = false;
        focus_loss_window = nullptr;
        if (!android_focus_sent)
          if (NuahNativeSession* session = bound_native_session())
            if (nuah_native_session_dispatch_window_focus(session, 1) != 0)
              android_focus_sent = true;
        (void)sync_mouse_lock(SDL_GetWindowFromID(event.window.windowID),
                              bound_native_session(), false);
        break;
      }
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
        emit(translated);
        /* Escape changes Roblox's MouseBehavior from the key callback.  Sober
         * re-evaluates that state on the same input turn before the following
         * menu click; do the query here instead of waiting for another motion
         * event (which may not exist when the user clicks immediately). */
        if (translated.android_keycode == NUAH_KEY_ESCAPE &&
            event.type == SDL_EVENT_KEY_DOWN && relative_mouse_mode) {
          (void)sync_mouse_lock(SDL_GetKeyboardFocus(),
                                bound_native_session(), false);
        }
        /* Roblox's NativeInputInterface lock state is the authority for
         * whether the camera/menu owns pointer capture. The next motion/wheel
         * query applies that state; changing SDL in a key event can move the
         * Wayland pointer out of this surface. */
        ++count;
        break;
      }
      case SDL_EVENT_MOUSE_MOTION:
        {
        SDL_Window* event_window =
            SDL_GetWindowFromID(event.motion.windowID);
        if (coalesce_motion) {
          if (!motion_lock_synced) {
            motion_lock_synced = true;
            if (sync_mouse_lock(event_window, bound_native_session(), true))
              break;
          }
        } else if (sync_mouse_lock(event_window, bound_native_session(), true)) {
          break;
        }
        translated.type = NUAH_INPUT_POINTER_MOTION;
        translated.action = 2;
        translated.dx = event.motion.xrel;
        translated.dy = event.motion.yrel;
        /* SDL's relative deltas are in the maximized host window's space.
         * Scale them to the locked Android surface before they reach
         * MotionEvent/Roblox, otherwise camera motion becomes too fast and
         * edge clicks stop lining up after KDE maximize. */
        map_host_delta_to_surface(event_window, &translated.dx,
                                  &translated.dy);
        if (!relative_mouse_mode && suppress_next_absolute_motion) {
          suppress_next_absolute_motion = false;
          /* Keep the last virtual position. Do not turn the compositor's
           * release notification into a Roblox mouse move. */
          break;
        }
        if (relative_mouse_mode) {
          /* SDL relative mode continues producing xrel/yrel at the window
           * edge.  Mirror Sober's captured-pointer path: keep a virtual
           * position and accumulate the relative axes instead of snapping
           * the coordinates back to the window centre on every event. */
          pointer_x += translated.dx;
          pointer_y += translated.dy;
          pointer_position_valid = true;
        } else {
          pointer_x = event.motion.x;
          pointer_y = event.motion.y;
          pointer_position_valid = true;
        }
        double surface_x = pointer_x;
        double surface_y = pointer_y;
        surface_pointer_position(event_window, &surface_x, &surface_y);
        translated.x = surface_x;
        translated.y = surface_y;
        /* Wayland emits an initial cursor-position notification while the
         * SDL surface is being realized. Sober's Android adapter only calls
         * nativePassMouseMove for an actual motion/captured-pointer delta;
         * forwarding this (0,0) bootstrap event can reach Roblox before its
         * input state exists and corrupt its render startup. */
        if (translated.dx == 0.0 && translated.dy == 0.0) break;
        if (drop_mouse_motion) break;
        if (!coalesce_motion) {
          emit(translated);
          ++count;
        } else if (!has_pending_motion) {
          pending_motion = translated;
          has_pending_motion = true;
        } else {
          pending_motion.dx += translated.dx;
          pending_motion.dy += translated.dy;
          pending_motion.x = translated.x;
          pending_motion.y = translated.y;
          pending_motion.timestamp_ns = translated.timestamp_ns;
        }
        break;
        }
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
      {
        /* Match Android's generic-motion listener for button transitions, but
         * never discard a button event when a pointer constraint is being
         * acquired. */
        SDL_Window* event_window =
            SDL_GetWindowFromID(event.button.windowID);
        const bool button_down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const int event_button = static_cast<int>(event.button.button);
        const bool was_relative = relative_mouse_mode;
        /* If a menu just opened, no motion event is guaranteed to arrive
         * between Escape and this click.  Reconcile an already-active lock
         * before translating the edge, but never acquire capture from a click
         * when SDL is currently absolute. */
        if (relative_mouse_mode)
          (void)sync_mouse_lock(event_window, bound_native_session(), false);
        translated.type = NUAH_INPUT_POINTER_BUTTON;
        translated.action = button_down ? 1 : 0;
        translated.button = event_button;
        /* Sober's Android adapter never takes the button event coordinates
         * as a new pointer position: y(MotionEvent) uses the last position
         * maintained by move/hover (f19785m/f19786n) for both press and
         * release.  Wayland may report a compositor-transition button at
         * (0,0); accepting that sample here makes an ordinary click appear
         * to teleport the Roblox pointer.  Seed only the very first event,
         * then leave pointer_x/pointer_y owned by motion. */
        if (!pointer_position_valid || (was_relative && !relative_mouse_mode)) {
          pointer_x = event.button.x;
          pointer_y = event.button.y;
          pointer_position_valid = true;
        }
        double event_x = pointer_x;
        double event_y = pointer_y;
        /* Match Sober's last-position semantics, but keep discrete edges
         * inside the Android surface.  Relative camera motion is allowed to
         * accumulate outside the view; a menu/fire hit-test is not.  Never
         * enter SDL relative mode from a click: Wayland may warp the host
         * pointer during that transition and the click then appears at 0,0. */
        surface_pointer_position(event_window, &event_x, &event_y);
        translated.x = event_x;
        translated.y = event_y;
        const unsigned int bit = button_bit(translated.button);
        if (button_down)
          buttons_down |= bit;
        else
          buttons_down &= ~bit;
        /* Deliver the click exactly as Sober does.  Capture is synchronized
         * from Roblox's lock-state callback on motion/focus events; changing
         * SDL's mode here can swallow the click or leave aim/fire stuck. */
        emit(translated);
        ++count;
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL: {
        /* Match Android's generic-motion ordering: the first event that
         * requests pointer capture is dropped by the View; the next wheel is
         * delivered after the constraint is active. */
        if (sync_mouse_lock(SDL_GetWindowFromID(event.wheel.windowID),
                            bound_native_session(), true))
          break;
        translated.type = NUAH_INPUT_POINTER_WHEEL;
        translated.action = 2;
        if (!pointer_position_valid) {
          pointer_x = event.wheel.mouse_x;
          pointer_y = event.wheel.mouse_y;
          pointer_position_valid = true;
        }
        double event_x = pointer_x;
        double event_y = pointer_y;
        SDL_Window* event_window =
            SDL_GetWindowFromID(event.wheel.windowID);
        surface_pointer_position(event_window, &event_x, &event_y);
        translated.x = event_x;
        translated.y = event_y;
        translated.dx = event.wheel.x;
        translated.dy = event.wheel.y;
        emit(translated);
        ++count;
        break;
      }
      default:
        break;
    }
  }
  flush_motion();
  service_focus_loss();
  if (SDL_Window* focus = SDL_GetMouseFocus(); focus) {
    set_host_cursor_hidden(true);
  }
  /* SDL_GetMouseState is not authoritative while a Wayland relative-pointer
   * constraint is active: some compositors report a cleared button mask even
   * though the physical button remains down.  Polling it here would inject a
   * synthetic release after a valid press and make FPS shooting stop.  Focus
   * transitions already release held buttons; keep reconciliation opt-in for
   * compositor diagnostics only. */
  if (!focus_loss_pending) {
    const char* reconcile = std::getenv("NUAH_RECONCILE_BUTTONS");
    if (reconcile && *reconcile && std::strcmp(reconcile, "0") != 0)
      reconcile_mouse_buttons(SDL_GetMouseFocus(), SDL_GetTicksNS());
  }
  if (pump_start != 0)
    nuah_perf_record_input(SDL_GetTicksNS() - pump_start,
                           static_cast<unsigned int>(count));
  return count;
}

extern "C" int nuah_input_quit_requested(void) {
  return quit_requested.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" void nuah_input_reset_quit(void) {
  quit_requested.store(false, std::memory_order_release);
}
