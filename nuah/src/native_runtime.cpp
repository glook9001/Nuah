#include "nuah/atl_backend.hpp"
#include "nuah/apk_loader.hpp"
#include "nuah/jni_contract.h"
#include "nuah/jni_runtime.h"
#include "nuah/input_bridge.h"

#include <dlfcn.h>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
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

void prepend_library_path(const std::filesystem::path& directory) {
  std::string value = directory.string();
  if (const char* existing = std::getenv("LD_LIBRARY_PATH");
      existing && *existing) {
    value += ":";
    value += existing;
  }
  if (::setenv("LD_LIBRARY_PATH", value.c_str(), 1) != 0) {
    throw std::runtime_error("cannot expose Nuah Android ABI providers");
  }
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
  prepend_library_path(NUAH_ANDROID_LIBRARY_DIR);
  constexpr const char* kMember = "lib/x86_64/libroblox.so";
  auto image = load_apk_library(image_apk, kMember);
  auto* jni_on_load = reinterpret_cast<std::int32_t (*)(JavaVM*, void*)>(
      ::dlsym(image.handle(), "JNI_OnLoad"));
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
            << image_apk << " into sealed memfd=" << image.fd()
            << " (" << image.size() << " bytes)\n";
  std::cerr << "nuah native: JNI_OnLoad accepted version 0x" << std::hex
            << jni_version << std::dec << "; registered natives="
            << nuah_jni_registered_count() << '\n';
  nuah_input_bind_jni_runtime(jni_runtime);
  nuah_jni_runtime_destroy(jni_runtime);
  std::cerr << "nuah native: game lifecycle/window loop is not yet connected\n";
  return 78;
}

}  // namespace nuah
