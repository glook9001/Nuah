// Compact SHA-256 (FIPS 180-4). Single-shot, no heap in transform.
#include "nuah/sha256.h"

#include <cstring>

namespace nuah {
namespace {
inline std::uint32_t rotr(std::uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

struct Ctx {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::uint64_t total = 0;
  unsigned char block[64]{};
  std::size_t used = 0;
};

void transform(Ctx& c, const unsigned char* p) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(p[i * 4 + 3]);
  for (int i = 16; i < 64; ++i) {
    std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t h[8];
  std::memcpy(h, c.h, sizeof(h));
  for (int i = 0; i < 64; ++i) {
    std::uint32_t s1 = rotr(h[4], 6) ^ rotr(h[4], 11) ^ rotr(h[4], 25);
    std::uint32_t ch = (h[4] & h[5]) ^ (~h[4] & h[6]);
    std::uint32_t t1 = h[7] + s1 + ch + kK[i] + w[i];
    std::uint32_t s0 = rotr(h[0], 2) ^ rotr(h[0], 13) ^ rotr(h[0], 22);
    std::uint32_t maj = (h[0] & h[1]) ^ (h[0] & h[2]) ^ (h[1] & h[2]);
    std::uint32_t t2 = s0 + maj;
    h[7] = h[6];
    h[6] = h[5];
    h[5] = h[4];
    h[4] = h[3] + t1;
    h[3] = h[2];
    h[2] = h[1];
    h[1] = h[0];
    h[0] = t1 + t2;
  }
  for (int i = 0; i < 8; ++i) c.h[i] += h[i];
}

void update(Ctx& c, const unsigned char* data, std::size_t size) {
  c.total += size;
  while (size > 0) {
    std::size_t room = 64 - c.used;
    std::size_t take = size < room ? size : room;
    std::memcpy(c.block + c.used, data, take);
    c.used += take;
    data += take;
    size -= take;
    if (c.used == 64) {
      transform(c, c.block);
      c.used = 0;
    }
  }
}
}  // namespace

std::string sha256_hex(const unsigned char* data, std::size_t size) {
  Ctx c;
  if (size > 0) update(c, data, size);
  std::uint64_t bits = c.total * 8;
  unsigned char pad = 0x80;
  update(c, &pad, 1);
  unsigned char zero = 0;
  while (c.used != 56) update(c, &zero, 1);
  unsigned char len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>((bits >> ((7 - i) * 8)) & 255);
  update(c, len, 8);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (int i = 0; i < 8; ++i)
    for (int s = 28; s >= 0; s -= 4) out.push_back(kHex[(c.h[i] >> s) & 15]);
  return out;
}
}  // namespace nuah
