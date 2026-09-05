#include "nuah/input_bridge.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "nuah/native_session.h"
#include "nuah/perf_metrics.h"
namespace {
std::atomic<NuahInputSink> sink{nullptr};
std::atomic<void*> sink_data{nullptr};
std::atomic<bool> quit_requested{false};
/* Captured-pointer path reports no absolute position. Like Sober's kl.e, keep
 * last position and accumulate AXIS_RELATIVE_X/Y before calling Roblox. */
double pointer_x = 0.0;
double pointer_y = 0.0;
bool pointer_position_valid = false;
bool relative_mouse_mode = false;
/* One absolute sample on relative-constraint release is a compositor
 * transition, not input; forwarding it would clobber capture coords (0,0). */
bool suppress_next_absolute_motion = false;
/* Focus is state, not stream. Wayland blips during resize/mode change must
 * not become Activity false/true (leaves input stuck until Alt-Tab). */
bool android_focus_sent = false;
SDL_Cursor* invisible_cursor = nullptr;
/* SDL may drop button-up across focus transitions; synthesize releases. */
unsigned int buttons_down = 0;
bool focus_loss_pending = false;
SDL_Window* focus_loss_window = nullptr;
Uint64 focus_loss_deadline_ns = 0;
/* FOCUS_LOST is not an activity pause on KDE/pointer-constraints; only a
 * hidden/minimized window is a real activity loss. */
bool window_mapped_for_android(SDL_Window* window) {
  if (!window) return false;
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
}
bool env_is_on_def(const char* name, bool dflt) {
  const char* v = std::getenv(name);
  return !v || !*v ? dflt : std::strcmp(v, "0") != 0;
}
bool env_is_on(const char* name) { return env_is_on_def(name, false); }
bool mouse_capture_enabled() {
  static const bool enabled = env_is_on_def("NUAH_MOUSE_CAPTURE", true);
  return enabled;
}
bool input_trace_enabled() {
  static const bool enabled = env_is_on("NUAH_INPUT_TRACE");
  return enabled;
}
#define INPUT_TRACE(...)                                    \
  do {                                                      \
    if (input_trace_enabled()) std::fprintf(stderr, __VA_ARGS__); \
  } while (0)
bool set_relative_mouse_mode(SDL_Window* window, bool enabled);
void release_pressed_buttons(SDL_Window* window, unsigned long long timestamp);
void seed_pointer_once(double x, double y);
SDL_Cursor* get_invisible_cursor() {
  if (invisible_cursor) return invisible_cursor;
  // Monochrome zero-mask = transparent; process-local, installed only in mouse-lock.
  static const Uint8 pixels[8] = {};
  static const Uint8 mask[8] = {};
  invisible_cursor = SDL_CreateCursor(pixels, mask, 8, 8, 0, 0);
  return invisible_cursor;
}
void set_host_cursor_hidden(bool hidden) {
  if (hidden) {
    if (SDL_Cursor* cursor = get_invisible_cursor()) (void)SDL_SetCursor(cursor);
    (void)SDL_HideCursor();
  } else {
    (void)SDL_SetCursor(SDL_GetDefaultCursor());
    (void)SDL_ShowCursor();
  }
}
NuahNativeSession* bound_native_session() { return static_cast<NuahNativeSession*>(sink_data.load(std::memory_order_acquire)); }
/* Persistent capture follows Roblox nativeGetMainWindowIsMouseLockedCenter().
 * View requests capture from generic-motion; button edges never change it. */
bool sync_mouse_lock(SDL_Window* window, NuahNativeSession* session, bool discard_on_lock) {
  if (!window || !session) return false;
  if (!mouse_capture_enabled()) {
    if (relative_mouse_mode) set_relative_mouse_mode(window, false);
    return false;
  }
  const int locked = nuah_native_session_is_mouse_locked_center(session);
  if (locked < 0) return false;
  const bool should_capture = locked != 0;
  if (should_capture == relative_mouse_mode) return false;
  /* On unlock, release held buttons first (lost Android button-up would keep
   * firing). Never release on acquisition: held buttons stay held into lock. */
  if (!should_capture && buttons_down) release_pressed_buttons(window, SDL_GetTicksNS());
  const bool capture_changed = set_relative_mouse_mode(window, should_capture);
  /* Android's generic-motion path requests capture and drops that one event;
   * the following captured event is the first one sent to Roblox. */
  return discard_on_lock && should_capture && capture_changed;
}
bool set_relative_mouse_mode(SDL_Window* window, bool enabled) {
  if (!window) return false;
  if (relative_mouse_mode == enabled && SDL_GetWindowRelativeMouseMode(window) == enabled) return true;
  if (enabled) {
    suppress_next_absolute_motion = false;
    /* Keep last position like Sober (accumulate deltas); seed from SDL only
     * for the first event in the window. */
    if (!pointer_position_valid) {
      float mouse_x = 0.0f, mouse_y = 0.0f;
      (void)SDL_GetMouseState(&mouse_x, &mouse_y);
      seed_pointer_once(mouse_x, mouse_y);
    }
  }
  if (!enabled && relative_mouse_mode) suppress_next_absolute_motion = true;
  const bool relative_ok = SDL_SetWindowRelativeMouseMode(window, enabled);
  /* SDL may complete a Wayland pointer-constraint transition asynchronously;
   * use SDL's reported state rather than assuming a failed request changed
   * nothing.  This prevents the next lock query from dropping every motion
   * event while the modes disagree. */
  relative_mouse_mode = SDL_GetWindowRelativeMouseMode(window);
  // No mouse-grab fallback: on Wayland it steals focus / flaps capture.
  if (!relative_ok) INPUT_TRACE("nuah input: relative mouse mode %s failed: %s\n", enabled ? "on" : "off", SDL_GetError());
  INPUT_TRACE("nuah input: relative mouse mode %s\n", enabled ? "on" : "off");
  return relative_mouse_mode == enabled;
}
void emit(const NuahInputEvent& event) {
  const auto callback = sink.load(std::memory_order_acquire);
  if (callback) callback(&event, sink_data.load(std::memory_order_acquire));
}
unsigned int button_bit(int button) { return button >= 1 && button <= 31 ? (1u << (button - 1)) : 0u; }
bool host_size_ok(SDL_Window* w, int* x, int* y) { return w && SDL_GetWindowSize(w, x, y) > 0 && *x > 0 && *y > 0; }
void seed_pointer_once(double x, double y) {
  if (!pointer_position_valid) { pointer_x = x; pointer_y = y; pointer_position_valid = true; }
}
void emit_button(int button, int action, unsigned long long ts, double x, double y) {
  NuahInputEvent e{};
  e.type = NUAH_INPUT_POINTER_BUTTON; e.action = action; e.button = button; e.timestamp_ns = ts; e.x = x; e.y = y;
  emit(e);
}
/* Host maximized vs locked Android surface: translate host->surface only at boundary. */
bool locked_surface_dimensions(int* width, int* height) {
  if (!width || !height) return false;
  static const struct LockedDimensions { bool valid = false; int w = 0; int h = 0; } dims = [] {
    LockedDimensions res;
    const char* locked = std::getenv("NUAH_LOCK_SURFACE_SIZE");
    if (!locked || !*locked || std::strcmp(locked, "0") == 0) return res;
    const char* width_value = std::getenv("NUAH_SURFACE_WIDTH"); const char* height_value = std::getenv("NUAH_SURFACE_HEIGHT");
    if (!width_value || !height_value) return res;
    char* width_end = nullptr; char* height_end = nullptr;
    const long parsed_width = std::strtol(width_value, &width_end, 10), parsed_height = std::strtol(height_value, &height_end, 10);
    if (width_end == width_value || *width_end != '\0' || height_end == height_value || *height_end != '\0' || parsed_width <= 0 || parsed_height <= 0) return res;
    res.valid = true; res.w = static_cast<int>(parsed_width); res.h = static_cast<int>(parsed_height);
    return res;
  }();
  if (!dims.valid) return false;
  *width = dims.w; *height = dims.h;
  return true;
}
void map_host_delta_to_surface(SDL_Window* window, double* dx, double* dy) {
  if (!dx || !dy) return;
  int surface_width = 0, surface_height = 0, host_width = 0, host_height = 0;
  if (!locked_surface_dimensions(&surface_width, &surface_height) || !host_size_ok(window, &host_width, &host_height)) return;
  *dx *= static_cast<double>(surface_width) / static_cast<double>(host_width);
  *dy *= static_cast<double>(surface_height) / static_cast<double>(host_height);
}
/* Clamp only discrete button/wheel events to the surface, never the continuous pointer. */
void surface_pointer_position(SDL_Window* window, double* x, double* y) {
  if (!x || !y) return;
  int surface_width = 0, surface_height = 0;
  if (!locked_surface_dimensions(&surface_width, &surface_height)) return;
  if (relative_mouse_mode) {
    *x = static_cast<double>(surface_width) / 2.0;
    *y = static_cast<double>(surface_height) / 2.0;
    return;
  }
  int width = 0, height = 0;
  if (!host_size_ok(window, &width, &height)) return;
  *x = std::clamp(*x * static_cast<double>(surface_width) / static_cast<double>(width), 0.0, static_cast<double>(surface_width - 1));
  *y = std::clamp(*y * static_cast<double>(surface_height) / static_cast<double>(height), 0.0, static_cast<double>(surface_height - 1));
}
void release_pressed_buttons(SDL_Window* window, unsigned long long timestamp) {
  double event_x = pointer_x, event_y = pointer_y;
  surface_pointer_position(window, &event_x, &event_y);
  for (int button = 1; button <= 5; ++button) {
    const unsigned int bit = button_bit(button);
    if (!(buttons_down & bit)) continue;
    emit_button(button, 0, timestamp, event_x, event_y);
    buttons_down &= ~bit;
  }
  /* Focus loss also ends capture; next locked motion re-requests it. */
  (void)set_relative_mouse_mode(window, false);
}
/* Reconcile SDL physical mask after compositor settles (buttons 1-5 only). */
void reconcile_mouse_buttons(SDL_Window* window, unsigned long long timestamp) {
  if (!window || !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) return;
  float host_x = 0.0f, host_y = 0.0f;
  const SDL_MouseButtonFlags host_buttons = SDL_GetMouseState(&host_x, &host_y);
  if (!relative_mouse_mode) seed_pointer_once(host_x, host_y);
  for (int button = 1; button <= 5; ++button) {
    const unsigned int bit = button_bit(button);
    const bool host_down = (host_buttons & SDL_BUTTON_MASK(button)) != 0;
    const bool sent_down = (buttons_down & bit) != 0;
    if (host_down == sent_down) continue;
    emit_button(button, host_down ? 1 : 0, timestamp, pointer_x, pointer_y);
    if (host_down) buttons_down |= bit;
    else buttons_down &= ~bit;
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
    if (NuahNativeSession* session = bound_native_session()) (void)nuah_native_session_dispatch_window_focus(session, 0);
    android_focus_sent = false;
  }
  focus_loss_pending = false;
  focus_loss_window = nullptr;
}
void arm_focus_loss(SDL_Window* window) {
  focus_loss_pending = true;
  focus_loss_window = window;
  focus_loss_deadline_ns = SDL_GetTicksNS();
}
void note_focus_gained(SDL_Window* window) {
  set_host_cursor_hidden(true);
  focus_loss_pending = false;
  focus_loss_window = nullptr;
  if (!android_focus_sent)
    if (NuahNativeSession* session = bound_native_session())
      if (nuah_native_session_dispatch_window_focus(session, 1) != 0) android_focus_sent = true;
  (void)sync_mouse_lock(window, bound_native_session(), false);
}
bool sync_lock_changed(SDL_Window* window) { return sync_mouse_lock(window, bound_native_session(), true); }
// Table-driven window-event dispatch: collapses HIDDEN/MINIMIZED/FOCUS_LOST/
// SHOWN/RESTORED/FOCUS_GAINED if-chains into kWinOp (1=unmapped, 2=maybe, 3=gain).
static constexpr struct { Uint32 type; Uint8 op; } kWinOp[] = {{SDL_EVENT_WINDOW_HIDDEN, 1}, {SDL_EVENT_WINDOW_MINIMIZED, 1}, {SDL_EVENT_WINDOW_FOCUS_LOST, 2}, {SDL_EVENT_WINDOW_SHOWN, 3}, {SDL_EVENT_WINDOW_RESTORED, 3}, {SDL_EVENT_WINDOW_FOCUS_GAINED, 3}};
bool handle_window_event(const SDL_Event& e) {
  Uint8 op = 0;
  for (const auto& k : kWinOp)
    if (k.type == e.type) op = k.op;
  if (!op) return false;
  SDL_Window* w = SDL_GetWindowFromID(e.window.windowID);
  if (op == 1) { arm_focus_loss(w); INPUT_TRACE("nuah input: host window unmapped\n"); }
  // Mapped toplevel losing keyboard focus is still the game surface.
  else if (op == 2) { if (!window_mapped_for_android(w)) arm_focus_loss(w); }
  else note_focus_gained(w);
  return true;
}
bool handle_quit_event(const SDL_Event& e) {
  if (e.type != SDL_EVENT_QUIT && e.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
  std::fprintf(stderr, "nuah input: %s window=%u\n", e.type == SDL_EVENT_QUIT ? "SDL_EVENT_QUIT" : "window close requested", e.window.windowID);
  release_pressed_buttons(SDL_GetWindowFromID(e.window.windowID), e.common.timestamp);
  quit_requested.store(true, std::memory_order_release);
  return true;
}
}  // namespace
extern "C" int nuah_android_keycode_from_ascii(int ascii) {
  if (ascii >= 'a' && ascii <= 'z') ascii += 'A' - 'a';
  if (ascii >= '0' && ascii <= '9') return ascii == '0' ? NUAH_KEY_0 : NUAH_KEY_1 + (ascii - '1');
  return (ascii >= 'A' && ascii <= 'Z') ? NUAH_KEY_A + (ascii - 'A') : NUAH_KEY_UNKNOWN;
}
extern "C" int nuah_android_keycode_from_scancode(int scancode) {
  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) return NUAH_KEY_A + (scancode - SDL_SCANCODE_A);
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) return NUAH_KEY_1 + (scancode - SDL_SCANCODE_1);
  if (scancode == SDL_SCANCODE_0) return NUAH_KEY_0;
  static constexpr struct { int sdl; int key; } kKeys[] = {
      {SDL_SCANCODE_UP, NUAH_KEY_DPAD_UP}, {SDL_SCANCODE_DOWN, NUAH_KEY_DPAD_DOWN}, {SDL_SCANCODE_LEFT, NUAH_KEY_DPAD_LEFT}, {SDL_SCANCODE_RIGHT, NUAH_KEY_DPAD_RIGHT},
      {SDL_SCANCODE_TAB, NUAH_KEY_TAB}, {SDL_SCANCODE_SPACE, NUAH_KEY_SPACE}, {SDL_SCANCODE_RETURN, NUAH_KEY_ENTER}, {SDL_SCANCODE_KP_ENTER, NUAH_KEY_ENTER},
      {SDL_SCANCODE_BACKSPACE, NUAH_KEY_DEL}, {SDL_SCANCODE_ESCAPE, NUAH_KEY_ESCAPE}, {SDL_SCANCODE_LSHIFT, NUAH_KEY_SHIFT_LEFT}, {SDL_SCANCODE_RSHIFT, NUAH_KEY_SHIFT_RIGHT},
      {SDL_SCANCODE_LCTRL, NUAH_KEY_CTRL_LEFT}, {SDL_SCANCODE_RCTRL, NUAH_KEY_CTRL_RIGHT}, {SDL_SCANCODE_LALT, NUAH_KEY_ALT_LEFT}, {SDL_SCANCODE_RALT, NUAH_KEY_ALT_RIGHT},
      {SDL_SCANCODE_LGUI, NUAH_KEY_META_LEFT}, {SDL_SCANCODE_RGUI, NUAH_KEY_META_RIGHT}, {SDL_SCANCODE_CAPSLOCK, NUAH_KEY_CAPS_LOCK}};
  for (const auto& e : kKeys) if (e.sdl == scancode) return e.key;
  return NUAH_KEY_UNKNOWN;
}
extern "C" int nuah_android_scancode_from_sdl(int scancode) {
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) return 2 + (scancode - SDL_SCANCODE_1);
  if (scancode == SDL_SCANCODE_0) return 11;
  static constexpr struct { int sdl; int linux; } kScan[] = {
      {SDL_SCANCODE_ESCAPE, 1}, {SDL_SCANCODE_BACKSPACE, 14}, {SDL_SCANCODE_TAB, 15}, {SDL_SCANCODE_Q, 16}, {SDL_SCANCODE_W, 17}, {SDL_SCANCODE_E, 18},
      {SDL_SCANCODE_R, 19}, {SDL_SCANCODE_T, 20}, {SDL_SCANCODE_Y, 21}, {SDL_SCANCODE_U, 22}, {SDL_SCANCODE_I, 23}, {SDL_SCANCODE_O, 24},
      {SDL_SCANCODE_P, 25}, {SDL_SCANCODE_RETURN, 28}, {SDL_SCANCODE_KP_ENTER, 28}, {SDL_SCANCODE_LCTRL, 29}, {SDL_SCANCODE_A, 30}, {SDL_SCANCODE_S, 31},
      {SDL_SCANCODE_D, 32}, {SDL_SCANCODE_F, 33}, {SDL_SCANCODE_G, 34}, {SDL_SCANCODE_H, 35}, {SDL_SCANCODE_J, 36}, {SDL_SCANCODE_K, 37},
      {SDL_SCANCODE_L, 38}, {SDL_SCANCODE_LSHIFT, 42}, {SDL_SCANCODE_Z, 44}, {SDL_SCANCODE_X, 45}, {SDL_SCANCODE_C, 46}, {SDL_SCANCODE_V, 47},
      {SDL_SCANCODE_B, 48}, {SDL_SCANCODE_N, 49}, {SDL_SCANCODE_M, 50}, {SDL_SCANCODE_RSHIFT, 54}, {SDL_SCANCODE_LALT, 56}, {SDL_SCANCODE_SPACE, 57},
      {SDL_SCANCODE_RCTRL, 97}, {SDL_SCANCODE_RALT, 100}, {SDL_SCANCODE_UP, 103}, {SDL_SCANCODE_LEFT, 105}, {SDL_SCANCODE_RIGHT, 106}, {SDL_SCANCODE_DOWN, 108}};
  for (const auto& e : kScan) if (e.sdl == scancode) return e.linux;
  return scancode;
}
unsigned int android_meta_state_from_sdl(SDL_Keymod modifiers) {
  if (__builtin_expect(!modifiers, 1)) return 0;
  static constexpr struct { SDL_Keymod sdl; unsigned int android; } kMods[] = {
      {SDL_KMOD_SHIFT, 0x1}, {SDL_KMOD_LSHIFT, 0x40}, {SDL_KMOD_RSHIFT, 0x80}, {SDL_KMOD_ALT, 0x2}, {SDL_KMOD_LALT, 0x10}, {SDL_KMOD_RALT, 0x20},
      {SDL_KMOD_CTRL, 0x1000}, {SDL_KMOD_LCTRL, 0x2000}, {SDL_KMOD_RCTRL, 0x4000}, {SDL_KMOD_GUI, 0x10000}, {SDL_KMOD_LGUI, 0x20000}, {SDL_KMOD_RGUI, 0x40000},
      {SDL_KMOD_CAPS, 0x100000}, {SDL_KMOD_NUM, 0x200000}, {SDL_KMOD_SCROLL, 0x400000},
  };
  unsigned int result = 0;
  for (const auto& m : kMods) if (modifiers & m.sdl) result |= m.android;
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
  // Pointer types are contiguous (MOTION..WHEEL): range check replaces 3-way OR.
  if (event->type == NUAH_INPUT_KEY)
    nuah_native_session_dispatch_key(session, event->android_keycode, event->action, event->repeat, event->physical_scancode, event->modifiers, event->timestamp_ns / 1000000ULL);
  else if (event->type >= NUAH_INPUT_POINTER_MOTION && event->type <= NUAH_INPUT_POINTER_WHEEL)
    nuah_native_session_dispatch_pointer_event(session, event->type, event->action, event->button, event->x, event->y, event->dx, event->dy, event->timestamp_ns / 1000000ULL);
}
}  // namespace
extern "C" void nuah_input_bind_native_session(NuahNativeSession* session) {
  nuah_input_set_sink(native_session_sink, session);
  // Runtime delivered initial focus before SDL pump; seed debounce from SDL.
  android_focus_sent = SDL_GetKeyboardFocus() != nullptr;
}
extern "C" void nuah_input_set_host_cursor_hidden(int hidden) { set_host_cursor_hidden(hidden != 0); }
extern "C" int nuah_input_pump(void) {
  const Uint64 pump_start = nuah_perf_trace_enabled() ? SDL_GetTicksNS() : 0;
  service_focus_loss();
  SDL_Event event{};
  int count = 0;
  static const bool nonblocking_events = env_is_on("NUAH_NONBLOCK_WAYLAND_EVENTS");
  /* Wayland can deliver hundreds of relative-pointer samples between two
   * 10-ms host pumps. Coalesce into one event with accumulated delta. */
  static const bool coalesce_motion = env_is_on_def("NUAH_INPUT_COALESCE", true);
  /* Diagnostic A/B only: drop camera deltas, keep buttons/wheel live. */
  static const bool drop_mouse_motion = env_is_on("NUAH_DROP_MOUSE_MOTION");
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
    return SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0;
  };
  auto push = [&](const NuahInputEvent& ev) { emit(ev); ++count; };
  while (next_event()) {
    if (event.type != SDL_EVENT_MOUSE_MOTION) {
      // Preserve SDL ordering: flush preceding motion before key/button/focus.
      flush_motion();
      motion_lock_synced = false;
    }
    if (handle_quit_event(event) || handle_window_event(event)) continue;
    NuahInputEvent translated{};
    translated.timestamp_ns = event.common.timestamp;
    switch (event.type) {
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP: {
        translated.type = NUAH_INPUT_KEY;
        const int sdl_scancode = static_cast<int>(event.key.scancode);
        translated.physical_scancode = nuah_android_scancode_from_sdl(sdl_scancode);
        translated.android_keycode = nuah_android_keycode_from_scancode(sdl_scancode);
        if (translated.android_keycode == NUAH_KEY_UNKNOWN) { translated.android_keycode = nuah_android_keycode_from_ascii(static_cast<int>(event.key.key)); }
        translated.action = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0;
        translated.repeat = event.key.repeat ? 1 : 0;
        translated.modifiers = android_meta_state_from_sdl(event.key.mod);
        push(translated);
        /* Escape exits mouse-lock from the key callback; re-query here so an
         * immediate menu click sees the new state without waiting for motion. */
        if (translated.android_keycode == NUAH_KEY_ESCAPE && event.type == SDL_EVENT_KEY_DOWN && relative_mouse_mode) (void)sync_mouse_lock(SDL_GetKeyboardFocus(), bound_native_session(), false);
        /* Roblox lock state is authoritative; SDL mode changes only on
         * motion/wheel/focus, never in a key event (Wayland warp risk). */
        break;
      }
      case SDL_EVENT_MOUSE_MOTION: {
        SDL_Window* event_window = SDL_GetWindowFromID(event.motion.windowID);
        if (coalesce_motion) {
          if (!motion_lock_synced) { motion_lock_synced = true; if (sync_lock_changed(event_window)) break; }
        } else if (sync_lock_changed(event_window)) break;
        translated.type = NUAH_INPUT_POINTER_MOTION;
        translated.action = 2;
        translated.dx = event.motion.xrel;
        translated.dy = event.motion.yrel;
        // Scale host-space deltas to locked surface (else fast camera / misaligned clicks).
        map_host_delta_to_surface(event_window, &translated.dx, &translated.dy);
        if (!relative_mouse_mode && suppress_next_absolute_motion) {
          suppress_next_absolute_motion = false;
          break;  // Compositor release sample; keep last virtual position.
        }
        if (relative_mouse_mode) { pointer_x += translated.dx; pointer_y += translated.dy; } else { pointer_x = event.motion.x; pointer_y = event.motion.y; }
        pointer_position_valid = true;
        double surface_x = pointer_x, surface_y = pointer_y;
        surface_pointer_position(event_window, &surface_x, &surface_y);
        translated.x = surface_x;
        translated.y = surface_y;
        /* Skip (0,0) bootstrap sample: Sober only forwards real motion deltas;
         * early zero can corrupt Roblox render startup. */
        if (translated.dx == 0.0 && translated.dy == 0.0) break;
        if (drop_mouse_motion) break;
        if (!coalesce_motion) push(translated); else if (!has_pending_motion) {
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
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        // Sync from lock-state on motion/focus only; never change SDL mode here.
        SDL_Window* event_window = SDL_GetWindowFromID(event.button.windowID);
        const bool button_down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const bool was_relative = relative_mouse_mode;
        translated.type = NUAH_INPUT_POINTER_BUTTON;
        translated.action = button_down ? 1 : 0;
        translated.button = static_cast<int>(event.button.button);
        /* Sober uses last motion position for press/release (never the event
         * sample, which Wayland may report as 0,0); seed only the first event. */
        if (!pointer_position_valid || (was_relative && !relative_mouse_mode)) { pointer_x = event.button.x; pointer_y = event.button.y; pointer_position_valid = true; }
        double event_x = pointer_x, event_y = pointer_y;
        /* Keep discrete edges inside the surface; never enter relative mode
         * from a click (Wayland warp would move it to 0,0). */
        surface_pointer_position(event_window, &event_x, &event_y);
        translated.x = event_x;
        translated.y = event_y;
        const unsigned int bit = button_bit(translated.button);
        if (button_down) buttons_down |= bit;
        else buttons_down &= ~bit;
        push(translated);
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL: {
        // First capture-requesting event is dropped by the View (Android ordering).
        if (sync_lock_changed(SDL_GetWindowFromID(event.wheel.windowID))) break;
        translated.type = NUAH_INPUT_POINTER_WHEEL;
        translated.action = 2;
        seed_pointer_once(event.wheel.mouse_x, event.wheel.mouse_y);
        double event_x = pointer_x, event_y = pointer_y;
        surface_pointer_position(SDL_GetWindowFromID(event.wheel.windowID), &event_x, &event_y);
        translated.x = event_x;
        translated.y = event_y;
        translated.dx = event.wheel.x;
        translated.dy = event.wheel.y;
        push(translated);
        break;
      }
      default:
        break;
    }
  }
  flush_motion();
  service_focus_loss();
  if (SDL_GetMouseFocus()) set_host_cursor_hidden(true);
  /* SDL_GetMouseState lies under an active relative constraint (cleared mask
   * with button held -> synthetic release breaks FPS fire). Reconcile opt-in
   * only; focus transitions already release. */
  if (!focus_loss_pending && env_is_on("NUAH_RECONCILE_BUTTONS")) reconcile_mouse_buttons(SDL_GetMouseFocus(), SDL_GetTicksNS());
  if (pump_start != 0) nuah_perf_record_input(SDL_GetTicksNS() - pump_start, static_cast<unsigned int>(count));
  return count;
}
extern "C" int nuah_input_quit_requested(void) { return quit_requested.load(std::memory_order_acquire) ? 1 : 0; }
extern "C" void nuah_input_reset_quit(void) { quit_requested.store(false, std::memory_order_release); }
