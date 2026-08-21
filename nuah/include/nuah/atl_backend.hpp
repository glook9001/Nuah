#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nuah {

struct NativeLaunchOptions {
  std::filesystem::path apk;
  std::vector<std::filesystem::path> split_apks;
  std::optional<std::string> activity;
  std::optional<std::string> uri;
  std::optional<std::filesystem::path> data_directory;
  int width = 1280;
  int height = 720;
  bool dimensions_explicit = false;
};

// Extracts x86_64 native libraries from ABI split APKs into the app-private
// lib directory System.loadLibrary expects.
std::filesystem::path prepare_app_native_libraries(
    const NativeLaunchOptions& options);

// Loads the x86_64 Roblox image through Nuah's native Sober-style boundary.
int run_native(const NativeLaunchOptions& options);

}  // namespace nuah
