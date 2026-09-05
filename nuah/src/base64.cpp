// Minimal RFC 4648 base64, no allocations on failure paths beyond result.
#include "nuah/base64.h"

namespace nuah {
namespace {
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int unbase(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

std::string base64_encode(const unsigned char* data, std::size_t size) {
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  for (std::size_t i = 0; i < size;) {
    unsigned triple = 0;
    int count = 0;
    for (int k = 0; k < 3; ++k) {
      triple <<= 8;
      if (i < size) {
        triple |= data[i++];
        ++count;
      }
    }
    out.push_back(kAlphabet[(triple >> 18) & 63]);
    out.push_back(kAlphabet[(triple >> 12) & 63]);
    out.push_back(count > 1 ? kAlphabet[(triple >> 6) & 63] : '=');
    out.push_back(count > 2 ? kAlphabet[triple & 63] : '=');
  }
  return out;
}

std::string base64_decode(const std::string& encoded) {
  if (encoded.empty()) return {};
  if (encoded.size() % 4 != 0) return {};
  std::string out;
  out.reserve((encoded.size() / 4) * 3);
  for (std::size_t i = 0; i < encoded.size(); i += 4) {
    int vals[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      char c = encoded[i + k];
      if (c == '=') {
        vals[k] = 0;
        ++pad;
      } else {
        vals[k] = unbase(c);
        if (vals[k] < 0) return {};
      }
    }
    if (pad > 2) return {};
    // Padding only valid at the very end.
    if (pad > 0 && i + 4 != encoded.size()) return {};
    unsigned triple =
        (static_cast<unsigned>(vals[0]) << 18) | (static_cast<unsigned>(vals[1]) << 12) |
        (static_cast<unsigned>(vals[2]) << 6) | static_cast<unsigned>(vals[3]);
    out.push_back(static_cast<char>((triple >> 16) & 255));
    if (pad < 2) out.push_back(static_cast<char>((triple >> 8) & 255));
    if (pad < 1) out.push_back(static_cast<char>(triple & 255));
  }
  return out;
}
}  // namespace nuah
