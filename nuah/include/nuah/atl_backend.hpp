#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nuah {

struct AtlLaunchOptions {
  std::filesystem::path apk;
  std::vector<std::filesystem::path> split_apks;
  std::optional<std::string> activity;
  std::optional<std::string> uri;
  std::optional<std::filesystem::path> data_directory;
  int width = 1280;
  int height = 720;
  bool dimensions_explicit = false;
};

using NativeLaunchOptions = AtlLaunchOptions;

// Extracts native libraries from ABI split APKs into ATL's application lib
// directory and returns the data-directory passed to ATL.
std::filesystem::path prepare_atl_native_libraries(
    const AtlLaunchOptions& options);

// Delivers a Roblox URI to ATL's already-running Android application over its
// GApplication D-Bus endpoint.  ATL turns that URI into a VIEW intent and
// resumes/creates the matching Android Activity in the existing ART process.
bool forward_atl_uri(const std::string& uri, std::string& error);

// Sets one HTTP Set-Cookie value through ATL's Android WebView CookieManager.
// The value never crosses a command line or a browser-database copy path.
bool forward_atl_cookie(const std::string& url, const std::string& value,
                        std::string& error);

// Replaces the current process with the full Android Translation Layer.
// The ATL process owns ART, the Android API implementation, and Activity lifecycle.
[[noreturn]] void exec_atl(const AtlLaunchOptions& options);

// Loads the x86_64 Roblox image through Nuah's native Sober-style boundary.
// This path never boots ART, OpenJDK, or an Android container: it creates the
// one native NuahJVM passed to Roblox through the libhybris-loaded image.
int run_native(const NativeLaunchOptions& options);

}  // namespace nuah
