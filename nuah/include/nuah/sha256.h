#pragma once
// Minimal SHA-256 for APK content hashing. Replaces glib GChecksum
// (drops glib-2.0 from nuah_core). Public-domain style implementation.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nuah {
// Hex-encoded lowercase SHA-256 of data.
std::string sha256_hex(const unsigned char* data, std::size_t size);
inline std::string sha256_hex(const std::vector<std::byte>& data) { return sha256_hex(reinterpret_cast<const unsigned char*>(data.data()), data.size()); }
}  // namespace nuah
