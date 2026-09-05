#include "nuah/window_session.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_render.h>
#include <dlfcn.h>
#include <poll.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "nuah/input_bridge.h"
namespace {
using WaylandEglWindowCreate = void* (*)(void*, int, int);
using WaylandDisplayGetFd = int (*)(void*);
uint64_t monotonic_ms() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
bool env_is_on(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && std::strcmp(v, "0") != 0;
}
uint64_t env_u64(const char* name, uint64_t dflt) {
  const char* v = std::getenv(name);
  if (!v || !*v) return dflt;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(v, &end, 10);
  return (end != v && *end == '\0') ? parsed : dflt;
}
void* window_prop(SDL_Window* window, const char* name) { return window ? SDL_GetPointerProperty(SDL_GetWindowProperties(window), name, nullptr) : nullptr; }
bool window_size_ok(SDL_Window* window, int* w, int* h) { return window && SDL_GetWindowSize(window, w, h) > 0 && *w > 0 && *h > 0; }
void clear_bg(SDL_Renderer* r) {
  SDL_SetRenderDrawColor(r, 11, 12, 17, 255);
  SDL_RenderClear(r);
}
void show_and_raise(SDL_Window* w) {
  SDL_ShowWindow(w);
  (void)SDL_SetWindowFullscreen(w, false);
  (void)SDL_RestoreWindow(w);
  SDL_RaiseWindow(w);
}
#define NUAH_TRACE(...) do { if (trace) std::fprintf(stderr, __VA_ARGS__); } while (0)
#define NUAH_BOOTSTRAP_TRACE(...) do { if (const char* t = std::getenv("NUAH_BOOTSTRAP_TRACE"); t && *t) std::fprintf(stderr, __VA_ARGS__); } while (0)
void* create_wayland_egl_window(SDL_Window* window, int width, int height) {
  void* wayland_surface = window_prop(window, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER);
  if (!wayland_surface) return nullptr;
  static void* library = ::dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
  static auto create = reinterpret_cast<WaylandEglWindowCreate>(library ? ::dlsym(library, "wl_egl_window_create") : nullptr);
  return create ? create(wayland_surface, width, height) : nullptr;
}
bool nonblocking_wayland_events() { return env_is_on("NUAH_NONBLOCK_WAYLAND_EVENTS"); }  // Default off: non-blocking probe races Mesa WSI (20-37 FPS vs steady 60).
bool wayland_display_readable(SDL_Window* window) {
  void* display = window_prop(window, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER);
  if (!display) return true;
  static void* library = ::dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
  static auto get_fd = reinterpret_cast<WaylandDisplayGetFd>(library ? ::dlsym(library, "wl_display_get_fd") : nullptr);
  if (!get_fd) return true;
  const int fd = get_fd(display);
  if (fd < 0) return true;
  struct pollfd descriptor{fd, POLLIN | POLLERR | POLLHUP, 0};
  const int result = ::poll(&descriptor, 1, 0);
  return result > 0 && descriptor.revents != 0;
}
}  // namespace
struct NuahWindowSession {
  SDL_Window* host = nullptr;
  SDL_Window* loading_window = nullptr;
  SDL_Renderer* loading_renderer = nullptr;
  NuahNativeWindow* native = nullptr;
  bool close_requested = false;
  bool loading_visible = false;
  bool surface_size_locked = false;
  int locked_surface_width = 0;
  int locked_surface_height = 0;
  uint64_t loading_started_ms = 0;
};
namespace {
void render_loading_frame(NuahWindowSession* session) {
  if (!session || !session->loading_window || !session->loading_renderer) return;
  int width = 0, height = 0;
  if (!window_size_ok(session->loading_window, &width, &height)) return;
  clear_bg(session->loading_renderer);
  const float cx = static_cast<float>(width) * 0.5f, cy = static_cast<float>(height) * 0.5f;
  const float card_w = std::min(420.0f, static_cast<float>(width) * 0.72f), card_h = 132.0f;
  const SDL_FRect card{cx - card_w * 0.5f, cy - card_h * 0.5f, card_w, card_h};
  SDL_SetRenderDrawColor(session->loading_renderer, 35, 36, 44, 255);
  SDL_RenderFillRect(session->loading_renderer, &card);
  const uint64_t elapsed = monotonic_ms() - session->loading_started_ms;
  const float phase = static_cast<float>(elapsed % 1200ULL) / 1200.0f;
  const float radius = 26.0f;
  for (int i = 0; i < 12; ++i) {
    const float angle = (static_cast<float>(i) / 12.0f + phase) * 6.2831853f;
    const float fade = 0.22f + 0.78f * (static_cast<float>((i + static_cast<int>(elapsed / 100ULL)) % 12) / 11.0f);
    SDL_SetRenderDrawColor(session->loading_renderer, static_cast<Uint8>(221.0f * fade), static_cast<Uint8>(128.0f * fade), 44, 255);
    SDL_RenderLine(session->loading_renderer, cx + std::cos(angle) * (radius - 8.0f), cy + std::sin(angle) * (radius - 8.0f), cx + std::cos(angle) * radius, cy + std::sin(angle) * radius);
  }
  SDL_RenderPresent(session->loading_renderer);
}
void destroy_loading_frame(NuahWindowSession* session) {
  if (!session) return;
  if (session->loading_renderer) { SDL_DestroyRenderer(session->loading_renderer); session->loading_renderer = nullptr; }
  if (session->loading_window) { SDL_HideWindow(session->loading_window); SDL_DestroyWindow(session->loading_window); session->loading_window = nullptr; }
  session->loading_visible = false;
}
/* xdg_toplevel needs a first commit to map; commit one host-owned clear while Roblox reaches vkQueuePresentKHR. */
void commit_initial_host_frame(SDL_Window* window) {
  if (!window) return;
  if (const char* e = std::getenv("NUAH_INITIAL_HOST_FRAME"); e && std::strcmp(e, "0") == 0) return;
  SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
  if (!renderer) { NUAH_BOOTSTRAP_TRACE("nuah window: initial host frame unavailable: %s\n", SDL_GetError()); return; }
  clear_bg(renderer);
  SDL_RenderPresent(renderer);
  SDL_DestroyRenderer(renderer);
  (void)SDL_SyncWindow(window);
  NUAH_BOOTSTRAP_TRACE("nuah window: initial host frame committed\n");
}
void current_surface_size(NuahWindowSession* session, int* w, int* h) {
  SDL_GetWindowSize(session->host, w, h);
  if (session->surface_size_locked && session->locked_surface_width > 0 && session->locked_surface_height > 0) { *w = session->locked_surface_width; *h = session->locked_surface_height; }
}
}  // namespace
extern "C" NuahWindowSession* nuah_window_session_create(int width, int height, const char* title) {
  if (width <= 0 || height <= 0) return nullptr;
  const bool trace = env_is_on("NUAH_BOOTSTRAP_TRACE");
  NUAH_TRACE("nuah window: SDL_Init\n");
  // Plain xdg_toplevel (no libdecor: avoids second decoration / input-only surface on KDE).
  static constexpr struct { const char* hint; const char* value; } kHints[] = {
      {SDL_HINT_VIDEO_DRIVER, "wayland"}, {SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0"}, {SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR, "0"}, {SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "1"}, {SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "1"}};
  for (const auto& h : kHints) SDL_SetHint(h.hint, h.value);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return nullptr;
  nuah_input_reset_quit();
  auto* session = new NuahWindowSession;
  session->surface_size_locked = env_is_on("NUAH_LOCK_SURFACE_SIZE");
  SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  /* Stay framed/floating: filling the output/work area is what KDE promotes to maximize/fullscreen. */
  const SDL_DisplayID display = SDL_GetPrimaryDisplay();
  SDL_Rect usable{};
  if (display && SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > 0) {
    constexpr int kMargin = 64;
    int fit_w = usable.w, fit_h = usable.h;
    if (width >= usable.w || height >= usable.h) { fit_w = std::max(320, usable.w - kMargin); fit_h = std::max(200, usable.h - kMargin); }
    if (width > fit_w) width = fit_w;
    if (height > fit_h) height = fit_h;
  }
  session->locked_surface_width = width; session->locked_surface_height = height;
  session->host = SDL_CreateWindow(title ? title : "Nuah", width, height, window_flags);
  NUAH_TRACE("nuah window: SDL_CreateWindow -> %p\n", static_cast<void*>(session->host));
  if (!session->host) {
    delete session;
    SDL_Quit();
    return nullptr;
  }
  if (trace) {
    const auto* video_driver = SDL_GetCurrentVideoDriver();
    const auto* wayland_surface = window_prop(session->host, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER);
    const auto* wayland_toplevel = window_prop(session->host, SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_POINTER);
    std::fprintf(stderr, "nuah window: video=%s flags=0x%llx size=%dx%d wayland_surface=%p wayland_toplevel=%p\n", video_driver ? video_driver : "unknown", static_cast<unsigned long long>(SDL_GetWindowFlags(session->host)), width, height, wayland_surface, wayland_toplevel);
  }
  // Map/raise so first frame/focus reach Roblox; sync initial commit before ART bootstrap; flush before JNI blocks (else ghost).
  show_and_raise(session->host);
  if (!SDL_SyncWindow(session->host)) NUAH_TRACE("nuah window: SDL_SyncWindow failed: %s\n", SDL_GetError());
  commit_initial_host_frame(session->host);
  SDL_PumpEvents();
  /* Capture realized size after first configure; with the lock this is the fixed render size. */
  int realized_width = 0, realized_height = 0;
  SDL_GetWindowSize(session->host, &realized_width, &realized_height);
  if (realized_width > 0 && realized_height > 0) {
    if (session->surface_size_locked) { session->locked_surface_width = realized_width; session->locked_surface_height = realized_height; }
    width = realized_width;
    height = realized_height;
  }
  // Hide host cursor; the Android/Roblox surface supplies its own.
  nuah_input_set_host_cursor_hidden(1);
  if (env_is_on("NUAH_WINDOW_ALWAYS_ON_TOP")) (void)SDL_SetWindowAlwaysOnTop(session->host, true);
  session->native = nuah_native_window_register_surface(session, session->host, width, height);
  NUAH_TRACE("nuah window: register -> %p\n", static_cast<void*>(session->native));
  if (!session->native) { SDL_DestroyWindow(session->host); delete session; SDL_Quit(); return nullptr; }
  void* egl_window = create_wayland_egl_window(session->host, width, height);
  NUAH_TRACE("nuah window: wayland egl window -> %p\n", egl_window);
  nuah_native_window_set_egl_handle(session->native, egl_window);
  nuah_native_window_set_default(session->native);
  return session;
}
extern "C" void nuah_window_session_destroy(NuahWindowSession* session) {
  if (!session) return;
  destroy_loading_frame(session);
  nuah_input_set_host_cursor_hidden(0);
  nuah_native_window_set_default(nullptr);
  nuah_native_window_unregister_surface(session);
  SDL_DestroyWindow(session->host);
  delete session;
  SDL_Quit();
}
extern "C" int nuah_window_session_should_close(const NuahWindowSession* session) { return !session || session->close_requested || nuah_input_quit_requested() ? 1 : 0; }
extern "C" void nuah_window_session_pump(NuahWindowSession* session) {
  if (!session) return;
  if (session->loading_visible) {
    static const uint64_t timeout_ms = env_u64("NUAH_LOADING_FRAME_TIMEOUT_MS", 30000);
    static const uint64_t minimum_ms = env_u64("NUAH_LOADING_FRAME_MIN_MS", 10000);
    const uint64_t elapsed_ms = monotonic_ms() - session->loading_started_ms;
    if (elapsed_ms >= minimum_ms || elapsed_ms >= timeout_ms) {
      NUAH_BOOTSTRAP_TRACE("nuah window: host loading frame hidden elapsed_ms=%llu\n", static_cast<unsigned long long>(elapsed_ms));
      destroy_loading_frame(session);
    } else
      render_loading_frame(session);
  }
  /* Keep façade dimensions current (consumed resize events); pump without consuming so input drains exactly once. */
  int width = 0, height = 0;
  current_surface_size(session, &width, &height);
  static int last_width = 0, last_height = 0;
  if (width > 0 && height > 0 && (width != last_width || height != last_height)) {
    last_width = width; last_height = height;
    nuah_native_window_update_geometry(session->native, width, height);
  }
  if (!nonblocking_wayland_events() || wayland_display_readable(session->host)) { SDL_PumpEvents(); }
}
extern "C" NuahNativeWindow* nuah_window_session_native_window(const NuahWindowSession* session) { return session ? session->native : nullptr; }
extern "C" void nuah_window_session_show_loading(NuahWindowSession* session) {
  if (!session || session->loading_visible || !session->host) return;
  if (!env_is_on("NUAH_LOADING_FRAME")) return;  // Host popup is opt-in diagnostics.
  int width = 0, height = 0;
  if (!window_size_ok(session->host, &width, &height)) return;
  session->loading_window = SDL_CreatePopupWindow(session->host, 0, 0, width, height, SDL_WINDOW_BORDERLESS | SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_POPUP_MENU);
  if (!session->loading_window) { NUAH_BOOTSTRAP_TRACE("nuah window: loading popup unavailable: %s\n", SDL_GetError()); return; }
  session->loading_renderer = SDL_CreateRenderer(session->loading_window, "software");
  if (!session->loading_renderer) { NUAH_BOOTSTRAP_TRACE("nuah window: loading renderer unavailable: %s\n", SDL_GetError()); destroy_loading_frame(session); return; }
  session->loading_started_ms = monotonic_ms();
  session->loading_visible = true;
  SDL_ShowWindow(session->loading_window);
  render_loading_frame(session);
  SDL_PumpEvents();
  NUAH_BOOTSTRAP_TRACE("nuah window: host loading frame shown\n");
}
extern "C" void nuah_window_session_hide_loading(NuahWindowSession* session) { destroy_loading_frame(session); }
