#pragma once
// Minimal RFC 4648 base64 encode/decode for Nuah cookie handoff.
// Replaces glib g_base64_* (drops glib-2.0 from nuah_core).
#include <cstddef>
#include <string>

namespace nuah {
// Returns empty string on invalid input (wrong padding/chars).
std::string base64_encode(const unsigned char* data, std::size_t size); std::string base64_decode(const std::string& encoded);
}  // namespace nuah
