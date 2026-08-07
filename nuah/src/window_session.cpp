#include "nuah/window_session.h"
#include "nuah/input_bridge.h"

#include <SDL3/SDL.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
using WaylandEglWindowCreate = void* (*)(void*, int, int);

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
}  // namespace

struct NuahWindowSession {
  SDL_Window* host = nullptr;
  NuahNativeWindow* native = nullptr;
  bool close_requested = false;
  std::string surface_token;
};

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
  SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "1");
  SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "1");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return nullptr;
  nuah_input_reset_quit();
  auto* session = new NuahWindowSession;
  session->surface_token = "nuah-surface";
  session->host = SDL_CreateWindow(
      title ? title : "Nuah", width, height,
      SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (trace)
    std::fprintf(stderr, "nuah window: SDL_CreateWindow -> %p\n",
                 static_cast<void*>(session->host));
  if (!session->host) {
    delete session;
    SDL_Quit();
    return nullptr;
  }
  // Wayland compositors may leave a newly-created SDL surface behind the
  // browser that launched it.  Explicitly map and raise the game window so
  // the first frame and keyboard focus are delivered to Roblox.
  SDL_ShowWindow(session->host);
  SDL_RaiseWindow(session->host);
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
  /* Keep the façade dimensions current even when the resize event is consumed
   * by the input bridge. Roblox queries ANativeWindow geometry during surface
   * and Vulkan setup, so this must be updated on every host pump. */
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(session->host, &width, &height);
  if (width > 0 && height > 0) {
    nuah_native_window_update_geometry(session->native, width, height);
  }
  /* SDL_PumpEvents refreshes the host queue without consuming it. The input
   * bridge drains that queue exactly once, so key/button events cannot be
   * duplicated or dropped by the window owner. */
  SDL_PumpEvents();
}

extern "C" NuahNativeWindow* nuah_window_session_native_window(
    const NuahWindowSession* session) {
  return session ? session->native : nullptr;
}
