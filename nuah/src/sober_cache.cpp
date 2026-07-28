#include "nuah/sober_cache.hpp"

#include "nuah/apk_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace nuah {
namespace {
std::filesystem::path default_source() {
  const char* home = std::getenv("HOME");
  if (!home) throw std::runtime_error("HOME is unavailable; provide a Sober package path");
  return std::filesystem::path(home) / ".var/app/org.vinegarhq.Sober/data/sober/packages/x86_64/com.roblox.client";
}
}

SoberCacheStatus inspect_sober_cache(const std::filesystem::path& requested) {
  SoberCacheStatus status{};
  status.source = requested.empty() ? default_source() : requested;
  status.base_apk = status.source / "base.apk";
  status.split_apk = status.source / "split_config.x86_64.apk";
  status.present = std::filesystem::is_regular_file(status.base_apk) && std::filesystem::is_regular_file(status.split_apk);
  if (!status.present) {
    status.message = "Sober's cached base.apk and x86_64 split were not found";
    return status;
  }
  try {
    const auto manifest = read_stored_apk_member(status.base_apk, "AndroidManifest.xml");
    const auto library = read_stored_apk_member(status.split_apk, "lib/x86_64/libroblox.so");
    status.structurally_valid = !manifest.bytes.empty() && !library.bytes.empty();
    status.message = status.structurally_valid
        ? "APK pair is structurally valid; APK v2/v3 signer verification is pending the dedicated verifier"
        : "APK pair is incomplete";
  } catch (const std::exception& error) {
    status.message = std::string("APK inspection failed: ") + error.what();
  }
  return status;
}

SoberCacheStatus adopt_sober_cache(const std::filesystem::path& destination, const std::filesystem::path& source) {
  auto status = inspect_sober_cache(source);
  if (!status.structurally_valid) return status;
  std::filesystem::create_directories(destination);
  std::filesystem::copy_file(status.base_apk, destination / "base.apk", std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(status.split_apk, destination / "split_config.x86_64.apk", std::filesystem::copy_options::overwrite_existing);
  status.message = "APK pair copied into Nuah managed storage; signer verification remains required before native execution";
  return status;
}
}  // namespace nuah
