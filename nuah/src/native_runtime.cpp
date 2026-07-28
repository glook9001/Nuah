#include "nuah/atl_backend.hpp"
#include "nuah/apk_loader.hpp"
#include "nuah/jni_contract.h"
#include "nuah/jni_runtime.h"
#include "nuah/input_bridge.h"
#include "nuah/window_session.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace nuah {
namespace {

std::vector<std::filesystem::path> image_candidates(
    const NativeLaunchOptions& options) {
  std::vector<std::filesystem::path> result;
  result.reserve(options.split_apks.size() + 1);
  result.push_back(options.apk);
  for (const auto& split : options.split_apks) result.push_back(split);
  if (options.split_apks.empty()) {
    const auto sibling = options.apk.parent_path() / "split_config.x86_64.apk";
    if (std::filesystem::is_regular_file(sibling)) result.push_back(sibling);
  }
  return result;
}

std::filesystem::path find_image(const NativeLaunchOptions& options) {
  constexpr const char* kMember = "lib/x86_64/libroblox.so";
  for (const auto& apk : image_candidates(options)) {
    if (!std::filesystem::is_regular_file(apk)) continue;
    try {
      (void)read_stored_apk_member(apk, kMember);
      return apk;
    } catch (const std::exception&) {
      // The base APK often contains no native image; continue with the ABI
      // split instead of treating that normal layout as a fatal error.
    }
  }
  throw std::runtime_error(
      "selected APK set has no x86_64 lib/x86_64/libroblox.so");
}

}  // namespace

int run_native(const NativeLaunchOptions& options) {
  if (!std::filesystem::is_regular_file(options.apk)) {
    throw std::runtime_error("native APK does not exist: " +
                             options.apk.string());
  }
  if (options.width <= 0 || options.height <= 0) {
    throw std::runtime_error("native window dimensions must be positive");
  }

  const auto image_apk = find_image(options);
  constexpr const char* kMember = "lib/x86_64/libroblox.so";
  auto image = load_apk_library(image_apk, kMember);
  auto* jni_on_load = reinterpret_cast<std::int32_t (*)(JavaVM*, void*)>(
      image.symbol("JNI_OnLoad"));
  if (!jni_on_load) {
    throw std::runtime_error(
        "loaded libroblox.so has no JNI_OnLoad entrypoint");
  }

  auto* jni_runtime = nuah_jni_runtime_create();
  if (!jni_runtime) throw std::runtime_error("cannot create Nuah JNI runtime");
  const auto jni_version = jni_on_load(nuah_jni_runtime_vm(jni_runtime), nullptr);
  if (jni_version != JNI_VERSION_1_6 && jni_version != JNI_VERSION_1_4 &&
      jni_version != JNI_VERSION_1_2) {
    nuah_jni_runtime_destroy(jni_runtime);
    throw std::runtime_error("Roblox JNI_OnLoad rejected Nuah JNI 1.6 runtime");
  }

  std::cerr << "nuah native: loaded " << kMember << " from "
            << image_apk << " via "
            << "libhybris Android loader and temporary ELF file " << image.path()
            << " (" << image.size() << " bytes)\n";
  std::cerr << "nuah native: JNI_OnLoad accepted version 0x" << std::hex
            << jni_version << std::dec << "; registered natives="
            << nuah_jni_registered_count() << '\n';
  nuah_input_bind_jni_runtime(jni_runtime);
  if (const char* interactive = std::getenv("NUAH_NATIVE_WINDOW_LOOP");
      interactive && std::string_view(interactive) == "1") {
    auto* window = nuah_window_session_create(options.width, options.height,
                                               "Nuah Roblox");
    if (!window) {
      nuah_jni_runtime_destroy(jni_runtime);
      throw std::runtime_error("cannot create Nuah native game window");
    }
    if (const char* initialize = std::getenv("NUAH_NATIVE_LIFECYCLE");
        initialize && std::string_view(initialize) == "1") {
      const auto handle = nuah_jni_runtime_initialize_game(
          jni_runtime, "com.roblox.client", options.data_directory
                                      ? options.data_directory->c_str()
                                      : "");
      if (handle != 0) {
        nuah_jni_runtime_dispatch_lifecycle(jni_runtime, "onStartNative");
        nuah_jni_runtime_dispatch_lifecycle(jni_runtime, "onResumeNative");
      }
    }
    std::uint64_t frames = 0;
    std::uint64_t max_frames = 0;
    if (const char* limit = std::getenv("NUAH_NATIVE_MAX_FRAMES");
        limit && *limit) max_frames = std::strtoull(limit, nullptr, 10);
    while (!nuah_window_session_should_close(window) &&
           (!max_frames || frames++ < max_frames)) {
      nuah_window_session_pump(window);
      nuah_input_pump();
      ::usleep(16000);
    }
    nuah_jni_runtime_dispatch_lifecycle(jni_runtime, "onPauseNative");
    nuah_jni_runtime_dispatch_lifecycle(jni_runtime, "onStopNative");
    nuah_window_session_destroy(window);
    nuah_jni_runtime_destroy(jni_runtime);
    return 0;
  }
  nuah_jni_runtime_destroy(jni_runtime);
  std::cerr << "nuah native: game lifecycle/window loop is not yet connected\n";
  return 78;
}

}  // namespace nuah
