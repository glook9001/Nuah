#include "nuah/input_bridge.h"
#include "nuah/jni_runtime.h"

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
  if (ascii >= '1' && ascii <= '9') return NUAH_KEY_1 + (ascii - '1');
  if (ascii >= 'a' && ascii <= 'z') ascii -= 'a' - 'A';
  if (ascii >= 'A' && ascii <= 'Z') return NUAH_KEY_A + (ascii - 'A');
  return NUAH_KEY_UNKNOWN;
}

extern "C" void nuah_input_set_sink(NuahInputSink callback, void* user_data) {
  sink_data.store(user_data, std::memory_order_release);
  sink.store(callback, std::memory_order_release);
}

namespace {
void jni_sink(const NuahInputEvent* event, void* user_data) {
  if (!event) return;
  auto* runtime = static_cast<NuahJniRuntime*>(user_data);
  if (event->type == NUAH_INPUT_KEY) {
    nuah_jni_runtime_dispatch_key(runtime, event->android_keycode,
                                  event->action, event->repeat,
                                  event->physical_scancode, event->modifiers,
                                  event->timestamp_ns / 1000000ULL);
  } else if (event->type == NUAH_INPUT_POINTER_MOTION ||
             event->type == NUAH_INPUT_POINTER_BUTTON ||
             event->type == NUAH_INPUT_POINTER_WHEEL) {
    nuah_jni_runtime_dispatch_pointer(
        runtime, event->action, event->button, event->x, event->y, event->dx,
        event->dy, event->timestamp_ns / 1000000ULL);
  }
}
}

extern "C" void nuah_input_bind_jni_runtime(NuahJniRuntime* runtime) {
  nuah_input_set_sink(jni_sink, runtime);
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
        translated.android_keycode =
            nuah_android_keycode_from_ascii(static_cast<int>(event.key.key));
        translated.physical_scancode = static_cast<int>(event.key.scancode);
        translated.action = event.type == SDL_EVENT_KEY_DOWN ? 1 : 0;
        translated.repeat = event.key.repeat ? 1 : 0;
        translated.modifiers = static_cast<unsigned int>(event.key.mod);
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
