#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nuah {

// A contract entry is evidence, not a guessed Android API.  `lookup` records
// an API Roblox asks Nuah to provide.  `register` records a native callback
// Roblox exposes through RegisterNatives.  A class of `*` means the existing
// capture did not retain that registration's owning class; it cannot be used
// to claim façade coverage until a dynamic trace resolves it.
struct JniManifestEntry {
  std::string kind;
  std::string class_name;
  std::string member;
  std::string signature;
  std::string source;
};

std::vector<JniManifestEntry> load_jni_manifest(const std::string& path);
void validate_jni_manifest(const std::vector<JniManifestEntry>& entries);
std::size_t jni_manifest_resolved_entries(
    const std::vector<JniManifestEntry>& entries);

}  // namespace nuah
