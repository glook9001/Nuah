#pragma once

#include <filesystem>
#include <string>

namespace nuah {

struct SoberCacheStatus {
  std::filesystem::path source;
  std::filesystem::path base_apk;
  std::filesystem::path split_apk;
  bool present = false;
  bool structurally_valid = false;
  std::string message;
};

SoberCacheStatus inspect_sober_cache(const std::filesystem::path& source = {});
SoberCacheStatus adopt_sober_cache(const std::filesystem::path& destination,
                                   const std::filesystem::path& source = {});

}  // namespace nuah
