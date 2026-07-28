#include "nuah/window_session.h"
#include "nuah/input_bridge.h"

#include <SDL3/SDL.h>

#include <string>

struct NuahWindowSession {
  SDL_Window* host = nullptr;
  NuahNativeWindow* native = nullptr;
  bool close_requested = false;
  std::string surface_token;
};

extern "C" NuahWindowSession* nuah_window_session_create(
    int width, int height, const char* title) {
  if (width <= 0 || height <= 0) return nullptr;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return nullptr;
  nuah_input_reset_quit();
  auto* session = new NuahWindowSession;
  session->surface_token = "nuah-surface";
  session->host = SDL_CreateWindow(
      title ? title : "Nuah", width, height,
      SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!session->host) {
    delete session;
    SDL_Quit();
    return nullptr;
  }
  session->native = nuah_native_window_register_surface(
      session, session->host, width, height);
  if (!session->native) {
    SDL_DestroyWindow(session->host);
    delete session;
    SDL_Quit();
    return nullptr;
  }
  return session;
}

extern "C" void nuah_window_session_destroy(NuahWindowSession* session) {
  if (!session) return;
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
  /* SDL_PumpEvents refreshes the host queue without consuming it. The input
   * bridge drains that queue exactly once, so key/button events cannot be
   * duplicated or dropped by the window owner. */
  SDL_PumpEvents();
}

extern "C" NuahNativeWindow* nuah_window_session_native_window(
    const NuahWindowSession* session) {
  return session ? session->native : nullptr;
}
