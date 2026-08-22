#include "nuah/window_session.h"
#include "nuah/input_bridge.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_render.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>

namespace {
using WaylandEglWindowCreate = void* (*)(void*, int, int);
using WaylandDisplayGetFd = int (*)(void*);

uint64_t monotonic_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void* create_wayland_egl_window(SDL_Window* window, int width, int height) {
  if (!window) return nullptr;
  const auto properties = SDL_GetWindowProperties(window);
  void* wayland_surface = SDL_GetPointerProperty(
      properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  if (!wayland_surface) return nullptr;
  static void* library =
      ::dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!library) return nullptr;
  static auto create = reinterpret_cast<WaylandEglWindowCreate>(
      ::dlsym(library, "wl_egl_window_create"));
  return create ? create(wayland_surface, width, height) : nullptr;
}

bool nonblocking_wayland_events() {
  const char* value = std::getenv("NUAH_NONBLOCK_WAYLAND_EVENTS");
  /* The non-blocking probe looks attractive, but on this Intel/Wayland path
   * it races Mesa's WSI display round-trip and starves the compositor's
   * configure/present traffic.  The measured result is 20--37 FPS with
   * 0.6--2 s gaps, versus a steady 60 FPS when the SDL pump is allowed to
   * drain Wayland normally.  Keep the measured path as the default and leave
   * NUAH_NONBLOCK_WAYLAND_EVENTS=1 as an explicit diagnostic A/B switch. */
  return value && *value && std::strcmp(value, "0") != 0;
}

bool wayland_display_readable(SDL_Window* window) {
  if (!window) return true;
  const auto properties = SDL_GetWindowProperties(window);
  void* display = SDL_GetPointerProperty(
      properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
  if (!display) return true;
  static void* library =
      ::dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
  static auto get_fd = reinterpret_cast<WaylandDisplayGetFd>(
      library ? ::dlsym(library, "wl_display_get_fd") : nullptr);
  if (!get_fd) return true;
  const int fd = get_fd(display);
  if (fd < 0) return true;
  struct pollfd descriptor {fd, POLLIN | POLLERR | POLLHUP, 0};
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
  std::string surface_token;
};

namespace {
void render_loading_frame(NuahWindowSession* session) {
  if (!session || !session->loading_window || !session->loading_renderer)
    return;
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(session->loading_window, &width, &height);
  if (width <= 0 || height <= 0) return;

  SDL_SetRenderDrawColor(session->loading_renderer, 11, 12, 17, 255);
  SDL_RenderClear(session->loading_renderer);

  /* A deliberately tiny host-owned loading frame. It is not a second
   * renderer for Roblox: it is a software popup held by the compositor while
   * the Android scene and shaders are built. */
  const float cx = static_cast<float>(width) * 0.5f;
  const float cy = static_cast<float>(height) * 0.5f;
  const float card_w = std::min(420.0f, static_cast<float>(width) * 0.72f);
  const float card_h = 132.0f;
  const SDL_FRect card{cx - card_w * 0.5f, cy - card_h * 0.5f, card_w, card_h};
  SDL_SetRenderDrawColor(session->loading_renderer, 35, 36, 44, 255);
  SDL_RenderFillRect(session->loading_renderer, &card);

  const uint64_t elapsed = monotonic_ms() - session->loading_started_ms;
  const float phase = static_cast<float>(elapsed % 1200ULL) / 1200.0f;
  const float radius = 26.0f;
  for (int i = 0; i < 12; ++i) {
    const float angle = (static_cast<float>(i) / 12.0f + phase) * 6.2831853f;
    const float fade = 0.22f + 0.78f *
        (static_cast<float>((i + static_cast<int>(elapsed / 100ULL)) % 12) / 11.0f);
    const Uint8 red = static_cast<Uint8>(221.0f * fade);
    const Uint8 green = static_cast<Uint8>(128.0f * fade);
    SDL_SetRenderDrawColor(session->loading_renderer, red, green, 44, 255);
    SDL_RenderLine(session->loading_renderer,
                   cx + std::cos(angle) * (radius - 8.0f),
                   cy + std::sin(angle) * (radius - 8.0f),
                   cx + std::cos(angle) * radius,
                   cy + std::sin(angle) * radius);
  }
  SDL_RenderPresent(session->loading_renderer);
}

void destroy_loading_frame(NuahWindowSession* session) {
  if (!session) return;
  if (session->loading_renderer) {
    SDL_DestroyRenderer(session->loading_renderer);
    session->loading_renderer = nullptr;
  }
  if (session->loading_window) {
    SDL_HideWindow(session->loading_window);
    SDL_DestroyWindow(session->loading_window);
    session->loading_window = nullptr;
  }
  session->loading_visible = false;
}

/* A Wayland xdg_toplevel is not guaranteed to become visible until its first
 * buffer is committed.  Native Roblox can spend several seconds before it
 * reaches vkQueuePresentKHR; during that gap SDL reports a valid window but
 * the compositor has nothing to map, which looks like a ghost window.  Commit
 * one host-owned clear with SDL's software renderer, then release it before
 * the Android/Vulkan path takes ownership.  This does not resize or persist a
 * second renderer. */
void commit_initial_host_frame(SDL_Window* window) {
  if (!window) return;
  const char* enabled = std::getenv("NUAH_INITIAL_HOST_FRAME");
  if (enabled && std::strcmp(enabled, "0") == 0) return;
  SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
  if (!renderer) {
    if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::fprintf(stderr, "nuah window: initial host frame unavailable: %s\n",
                   SDL_GetError());
    }
    return;
  }
  SDL_SetRenderDrawColor(renderer, 11, 12, 17, 255);
  SDL_RenderClear(renderer);
  SDL_RenderPresent(renderer);
  SDL_DestroyRenderer(renderer);
  (void)SDL_SyncWindow(window);
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace) {
    std::fprintf(stderr, "nuah window: initial host frame committed\n");
  }
}
}  // namespace

extern "C" NuahWindowSession* nuah_window_session_create(
    int width, int height, const char* title) {
  if (width <= 0 || height <= 0) return nullptr;
  const bool trace = [] {
    const char* value = std::getenv("NUAH_BOOTSTRAP_TRACE");
    return value && *value;
  }();
  if (trace) std::fprintf(stderr, "nuah window: SDL_Init\n");
  // A native launch can originate from a terminal or a WebKit child.  Ask
  // SDL/Wayland to activate the newly-created game surface instead of leaving
  // the previous error page in front of it.
  /* Keep the game as a plain xdg_toplevel.  Libdecor can create a second
   * decoration object before the first Vulkan commit; on some KDE sessions
   * that leaves the SDL surface mapped only as an input/thumbnail surface.
   * Roblox supplies its own Android-style surface, so server-side Wayland
   * decorations are the least surprising host boundary. */
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
  SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");
  SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR, "0");
  SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "1");
  SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "1");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return nullptr;
  nuah_input_reset_quit();
  auto* session = new NuahWindowSession;
  session->surface_token = "nuah-surface";
  const char* lock_surface = std::getenv("NUAH_LOCK_SURFACE_SIZE");
  session->surface_size_locked =
      lock_surface && std::strcmp(lock_surface, "0") != 0;
  SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                 SDL_WINDOW_HIGH_PIXEL_DENSITY;
  /* Stay a framed floating window.  A client size that fills the output or
   * the work area is what KDE promotes to maximize/fullscreen. */
  const SDL_DisplayID display = SDL_GetPrimaryDisplay();
  SDL_Rect usable{};
  if (display && SDL_GetDisplayUsableBounds(display, &usable) &&
      usable.w > 0 && usable.h > 0) {
    constexpr int kMargin = 64;
    int fit_w = usable.w;
    int fit_h = usable.h;
    if (width >= usable.w || height >= usable.h) {
      fit_w = std::max(320, usable.w - kMargin);
      fit_h = std::max(200, usable.h - kMargin);
    }
    if (width > fit_w) width = fit_w;
    if (height > fit_h) height = fit_h;
  }
  session->locked_surface_width = width;
  session->locked_surface_height = height;
  session->host = SDL_CreateWindow(title ? title : "Nuah", width, height,
                                   window_flags);
  if (trace)
    std::fprintf(stderr, "nuah window: SDL_CreateWindow -> %p\n",
                 static_cast<void*>(session->host));
  if (!session->host) {
    delete session;
    SDL_Quit();
    return nullptr;
  }
  if (trace) {
    const auto properties = SDL_GetWindowProperties(session->host);
    const auto* video_driver = SDL_GetCurrentVideoDriver();
    const auto* wayland_surface = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    const auto* wayland_toplevel = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_XDG_TOPLEVEL_POINTER, nullptr);
    std::fprintf(stderr,
                 "nuah window: video=%s flags=0x%llx size=%dx%d wayland_surface=%p wayland_toplevel=%p\n",
                 video_driver ? video_driver : "unknown",
                 static_cast<unsigned long long>(SDL_GetWindowFlags(session->host)),
                 width, height, wayland_surface, wayland_toplevel);
  }
  // Wayland compositors may leave a newly-created SDL surface behind the
  // browser that launched it.  Explicitly map and raise the game window so
  // the first frame and keyboard focus are delivered to Roblox.
  SDL_ShowWindow(session->host);
  (void)SDL_SetWindowFullscreen(session->host, false);
  (void)SDL_RestoreWindow(session->host);
  SDL_RaiseWindow(session->host);
  /* SDL queues the initial Wayland xdg_toplevel commit.  Synchronize it
   * before ART starts the synchronous Android surface bootstrap; otherwise
   * KDE can keep the newly-created toplevel behind the launching browser even
   * though SDL reports it as shown. */
  if (!SDL_SyncWindow(session->host) && trace)
    std::fprintf(stderr, "nuah window: SDL_SyncWindow failed: %s\n",
                 SDL_GetError());
  commit_initial_host_frame(session->host);
  /* Flush the initial xdg_toplevel commit before ART/Roblox starts its
   * synchronous surface bootstrap.  Without this, the compositor can defer
   * mapping until after the first frame while the native launch thread is
   * still blocked in JNI, which looks like a ghost window to the user. */
  SDL_PumpEvents();
  /* Wayland may subtract decorations or apply a compositor scale during the
   * first configure. Capture that realized size before creating the Android
   * façade. If the lock is enabled, this becomes the fixed render size for
   * the whole session; later desktop resizes must not trigger a Roblox
   * swapchain rebuild. */
  int realized_width = 0;
  int realized_height = 0;
  SDL_GetWindowSize(session->host, &realized_width, &realized_height);
  if (realized_width > 0 && realized_height > 0) {
    if (session->surface_size_locked) {
      session->locked_surface_width = realized_width;
      session->locked_surface_height = realized_height;
    }
    width = realized_width;
    height = realized_height;
  }
  /* The Android/Roblox surface supplies its own cursor.  Hide the Wayland/KDE
   * host cursor as soon as the game window is mapped; input capture remains a
   * separate state controlled by Roblox's mouse-lock callback. */
  nuah_input_set_host_cursor_hidden(1);
  if (const char* always_on_top = std::getenv("NUAH_WINDOW_ALWAYS_ON_TOP");
      always_on_top && *always_on_top && std::strcmp(always_on_top, "0") != 0) {
    (void)SDL_SetWindowAlwaysOnTop(session->host, true);
  }
  session->native = nuah_native_window_register_surface(
      session, session->host, width, height);
  if (trace)
    std::fprintf(stderr, "nuah window: register -> %p\n",
                 static_cast<void*>(session->native));
  if (!session->native) {
    SDL_DestroyWindow(session->host);
    delete session;
    SDL_Quit();
    return nullptr;
  }
  void* egl_window = create_wayland_egl_window(session->host, width, height);
  if (trace)
    std::fprintf(stderr, "nuah window: wayland egl window -> %p\n", egl_window);
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

extern "C" int nuah_window_session_should_close(
    const NuahWindowSession* session) {
  return !session || session->close_requested || nuah_input_quit_requested()
             ? 1
             : 0;
}

extern "C" void nuah_window_session_pump(NuahWindowSession* session) {
  if (!session) return;
  if (session->loading_visible) {
    static const uint64_t timeout_ms = [] {
      const char* timeout_value = std::getenv("NUAH_LOADING_FRAME_TIMEOUT_MS");
      uint64_t ms = 30000;
      if (timeout_value && *timeout_value) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(timeout_value, &end, 10);
        if (end != timeout_value && *end == '\0') ms = parsed;
      }
      return ms;
    }();
    static const uint64_t minimum_ms = [] {
      const char* minimum_value = std::getenv("NUAH_LOADING_FRAME_MIN_MS");
      uint64_t ms = 10000;
      if (minimum_value && *minimum_value) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(minimum_value, &end, 10);
        if (end != minimum_value && *end == '\0') ms = parsed;
      }
      return ms;
    }();
    const uint64_t elapsed_ms = monotonic_ms() - session->loading_started_ms;
    if (elapsed_ms >= minimum_ms || elapsed_ms >= timeout_ms) {
      if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::fprintf(stderr,
                     "nuah window: host loading frame hidden elapsed_ms=%llu\n",
                     static_cast<unsigned long long>(elapsed_ms));
      }
      destroy_loading_frame(session);
    } else {
      render_loading_frame(session);
    }
  }
  /* Keep the façade dimensions current even when the resize event is consumed
   * by the input bridge. Roblox queries ANativeWindow geometry during surface
   * and Vulkan setup, so this must be updated when dimensions change. */
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(session->host, &width, &height);
  if (session->surface_size_locked && session->locked_surface_width > 0 &&
      session->locked_surface_height > 0) {
    width = session->locked_surface_width;
    height = session->locked_surface_height;
  }
  static int last_width = 0;
  static int last_height = 0;
  if (width > 0 && height > 0 && (width != last_width || height != last_height)) {
    last_width = width;
    last_height = height;
    nuah_native_window_update_geometry(session->native, width, height);
  }
  /* SDL_PumpEvents refreshes the host queue without consuming it. The input
   * bridge drains that queue exactly once, so key/button events cannot be
   * duplicated or dropped by the window owner. */
  if (!nonblocking_wayland_events() ||
      wayland_display_readable(session->host)) {
    SDL_PumpEvents();
  }
}

extern "C" NuahNativeWindow* nuah_window_session_native_window(
    const NuahWindowSession* session) {
  return session ? session->native : nullptr;
}

extern "C" void nuah_window_session_show_loading(NuahWindowSession* session) {
  if (!session || session->loading_visible || !session->host) return;
  const char* enabled = std::getenv("NUAH_LOADING_FRAME");
  /* Keep the host popup opt-in. It is useful for diagnosing cold-start
   * transitions, but it is not part of the playable window and should never
   * affect normal frame-pacing evaluation. */
  if (!enabled || std::strcmp(enabled, "0") == 0) return;

  int width = 0;
  int height = 0;
  SDL_GetWindowSize(session->host, &width, &height);
  if (width <= 0 || height <= 0) return;
  session->loading_window = SDL_CreatePopupWindow(
      session->host, 0, 0, width, height,
      SDL_WINDOW_BORDERLESS | SDL_WINDOW_NOT_FOCUSABLE |
          SDL_WINDOW_POPUP_MENU);
  if (!session->loading_window) {
    if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::fprintf(stderr, "nuah window: loading popup unavailable: %s\n",
                   SDL_GetError());
    }
    return;
  }
  session->loading_renderer = SDL_CreateRenderer(
      session->loading_window, "software");
  if (!session->loading_renderer) {
    if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::fprintf(stderr, "nuah window: loading renderer unavailable: %s\n",
                   SDL_GetError());
    }
    destroy_loading_frame(session);
    return;
  }
  session->loading_started_ms = monotonic_ms();
  session->loading_visible = true;
  SDL_ShowWindow(session->loading_window);
  render_loading_frame(session);
  SDL_PumpEvents();
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace) {
    std::fprintf(stderr, "nuah window: host loading frame shown\n");
  }
}

extern "C" void nuah_window_session_hide_loading(NuahWindowSession* session) {
  destroy_loading_frame(session);
}
