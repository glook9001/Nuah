#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "nuah/android_abi_registry.h"
namespace {
using AssetBytes = std::shared_ptr<const std::vector<unsigned char>>;
/* Bounded positive cache: APK assets immutable but unbounded cache turns malformed/large APK into permanent RSS growth. */
constexpr std::size_t kPositiveCacheLimit = 64 * 1024 * 1024, kPositiveCacheEntryLimit = 16 * 1024 * 1024;
constexpr long long kMaxAssetBytes = 512LL * 1024 * 1024;  // Malformed APK probe must not force multi-hundred-MiB allocation.
struct NuahAssetManager { std::vector<std::string> apks, extracted_roots; std::mutex mutex;
  /* APK members immutable: remember misses so repeated probes skip ZIP rescans; retain bounded positives so render path skips re-decompress. Remote delivery stays Roblox-owned. */
  std::unordered_set<std::string> missing, loading; std::unordered_map<std::string, AssetBytes> cached; std::size_t cached_bytes = 0; };
struct NuahAsset { AssetBytes bytes; std::size_t offset = 0; };
std::once_flag manager_once; NuahAssetManager manager;
inline bool env_enabled(const char* n) { const char* v = std::getenv(n); return v && *v && std::strcmp(v, "0") != 0; } bool trace_enabled() { static const bool e = env_enabled("NUAH_BOOTSTRAP_TRACE"); return e; }
void append_colon_separated(const char* list, std::vector<std::string>& t) { if (!list || !*list) return; const char* b = list; for (const char* c = list;; ++c) { if (*c != ':' && *c != '\0') continue; if (c != b) t.emplace_back(b, c); if (*c == '\0') break; b = c + 1; } }
void initialize_manager() { append_colon_separated(std::getenv("NUAH_ASSET_ROOTS"), manager.extracted_roots);
  if (const char* d = std::getenv("ANDROID_APP_DATA_DIR"); d && *d) { manager.extracted_roots.emplace_back((std::filesystem::path(d) / "files/assets").string()); manager.extracted_roots.emplace_back((std::filesystem::path(d) / "assets").string()); }
  append_colon_separated(std::getenv("NUAH_APK_PATHS"), manager.apks); }
/* Direct ZIP reader: EOCD/central-directory + zlib like apk_loader (visit_apk_members/decode_member/inflate_raw), inlined so shim skips libarchive. No exceptions cross C ABI: corrupt/truncated/unsupported shapes are misses (nullptr) handled via missing-cache + ENOENT. */
constexpr std::uint32_t kZipEocdSig = 0x06054b50, kZipCentralSig = 0x02014b50, kZipLocalSig = 0x04034b50; constexpr std::uint16_t kZipStored = 0, kZipDeflated = 8; constexpr std::uint32_t kZipFlagEncrypted = 0x0001;  // Bit 3 (data descriptor): central sizes authoritative, no special-casing.
constexpr std::uint32_t kZip64Sentinel32 = 0xffffffffu; constexpr std::uint16_t kZip64Sentinel16 = 0xffffu; constexpr std::size_t kZipEocdMinSize = 22, kZipEocdSearchMax = 0xffffu + 22;
inline std::uint16_t zip_u16(const unsigned char* b, std::size_t o) { return static_cast<std::uint16_t>(b[o]) | (static_cast<std::uint16_t>(b[o + 1]) << 8); } inline std::uint32_t zip_u32(const unsigned char* b, std::size_t o) { return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24); }
// RAII munmap guard: read_member has many corrupt-APK early returns; fd closed right after mmap (mapping keeps pages), only mapping needs scope.
struct ScopedMmap { void* addr = nullptr; std::size_t len = 0; ScopedMmap() = default; ScopedMmap(const ScopedMmap&) = delete; ScopedMmap& operator=(const ScopedMmap&) = delete; ~ScopedMmap() { if (addr && addr != MAP_FAILED && len > 0) ::munmap(addr, len); } const unsigned char* data() const { return static_cast<const unsigned char*>(addr); } };
AssetBytes inflate_member(const unsigned char* c, std::size_t cs, std::size_t us) {
  if (us == 0) return std::make_shared<std::vector<unsigned char>>();  // Empty asset valid (zero-length probe); skip zlib so zero avail_out never bootstraps-fails.
  if (cs == 0) return nullptr;
  auto b = std::make_shared<std::vector<unsigned char>>(us); z_stream s{}; s.next_in = const_cast<Bytef*>(c); s.avail_in = static_cast<uInt>(cs); s.next_out = b->data(); s.avail_out = static_cast<uInt>(b->size());
  if (::inflateInit2(&s, -MAX_WBITS) != Z_OK) return nullptr;
  int st = ::inflate(&s, Z_FINISH); bool ok = (st == Z_STREAM_END && s.total_out == b->size() && s.total_in == cs); ::inflateEnd(&s); return ok ? b : nullptr; }
AssetBytes read_member(const std::string& apk, const std::string& member) {
  int fd = ::open(apk.c_str(), O_RDONLY | O_CLOEXEC); if (fd < 0) return nullptr; struct stat st{}; if (::fstat(fd, &st) != 0) { ::close(fd); return nullptr; }
  if (st.st_size < static_cast<off_t>(kZipEocdMinSize)) { ::close(fd); return nullptr; } auto size = static_cast<std::size_t>(st.st_size);
  void* addr = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0); ::close(fd); if (addr == MAP_FAILED) return nullptr;
  ScopedMmap mapped; mapped.addr = addr; mapped.len = size; ::madvise(addr, size, MADV_WILLNEED | MADV_SEQUENTIAL);  // Cold bootstrap: sequential + will-need keeps one readahead instead of fault-per-entry; advisory.
  const unsigned char* base = mapped.data();
  // EOCD at very end up to 64 KiB comment. Scan backwards so EOCD-like sig inside stored member or Signing Block cannot shadow real directory; take last occurrence.
  std::size_t first = size > kZipEocdSearchMax ? size - kZipEocdSearchMax : 0, eocd = size;  // sentinel: not found
  for (std::size_t i = size - kZipEocdMinSize;; --i) { if (zip_u32(base, i) == kZipEocdSig) { eocd = i; break; } if (i == first) break; }
  if (eocd == size) return nullptr;  // EOCD layout: sig(0)+disks(4..10)+entries(10,2)+cd_size(12,4)+cd_offset(16,4)+comment(20,2). Same offsets as apk_loader.
  auto entries = zip_u16(base, eocd + 10); auto central_offset = zip_u32(base, eocd + 16);
  if (entries == kZip64Sentinel16 || central_offset == kZip64Sentinel32) return nullptr;  // ZIP64 locator/0xFFFF/0xFFFFFFFF need ZIP64 extras; miss, not crash. Production APKs are ZIP32.
  if (central_offset > size) return nullptr;
  std::size_t pos = central_offset;
  for (std::uint16_t i = 0; i < entries; ++i) {
    if (pos + 46 > size || zip_u32(base, pos) != kZipCentralSig) return nullptr;  // Central header 46 bytes fixed + name/extra/comment.
    auto flags = zip_u16(base, pos + 8), method = zip_u16(base, pos + 10); auto cs = zip_u32(base, pos + 20), us = zip_u32(base, pos + 24);
    auto nl = zip_u16(base, pos + 28), el = zip_u16(base, pos + 30), cl = zip_u16(base, pos + 32); auto lo = zip_u32(base, pos + 42);
    std::size_t he = pos + 46; if (nl > size || el > size - nl || cl > size - nl - el) return nullptr;  // Overflow-safe next-entry on untrusted u16.
    std::size_t next = he + static_cast<std::size_t>(nl) + static_cast<std::size_t>(el) + static_cast<std::size_t>(cl); if (next < he || next > size) return nullptr;
    if (!(static_cast<std::size_t>(nl) == member.size() && std::memcmp(base + he, member.data(), nl) == 0)) { pos = next; continue; }
    if (cs == kZip64Sentinel32 || us == kZip64Sentinel32 || lo == kZip64Sentinel32 || (flags & kZipFlagEncrypted) != 0) return nullptr;  // Per-entry ZIP64/encryption: miss.
    if (static_cast<long long>(us) > kMaxAssetBytes || static_cast<std::size_t>(cs) > size) return nullptr;  // Cap declared size; bound compressed run so start+size cannot wrap.
    if (lo + 30 > size || zip_u32(base, lo) != kZipLocalSig) return nullptr;
    auto ln = zip_u16(base, lo + 26), le = zip_u16(base, lo + 28); if (ln > size || le > size - ln) return nullptr;
    std::size_t ds = static_cast<std::size_t>(lo) + 30 + static_cast<std::size_t>(ln) + static_cast<std::size_t>(le);
    if (ds < static_cast<std::size_t>(lo) || static_cast<std::size_t>(cs) > size - ds) return nullptr;
    if (method == kZipStored) { if (cs != us) return nullptr; auto b = std::make_shared<std::vector<unsigned char>>(us); if (!b->empty()) std::memcpy(b->data(), base + ds, cs); return b; }
    if (method == kZipDeflated) return inflate_member(base + ds, cs, us);
    return nullptr; /* unsupported method (bzip2/lzma): miss */ }
  return nullptr; }
AssetBytes read_extracted(const std::string& root, const std::string& member) { std::error_code e; auto p = std::filesystem::path(root) / member; if (!std::filesystem::is_regular_file(p, e) || e) return nullptr;
  std::ifstream in(p, std::ios::binary | std::ios::ate); if (!in) return nullptr; auto end = in.tellg(); if (end < 0 || end > std::streamoff(kMaxAssetBytes)) return nullptr;
  auto b = std::make_shared<std::vector<unsigned char>>(static_cast<std::size_t>(end)); in.seekg(0, std::ios::beg); if (!b->empty() && !in.read(reinterpret_cast<char*>(b->data()), static_cast<std::streamsize>(b->size()))) return nullptr; return b; }
std::unique_ptr<NuahAsset> make_asset(const AssetBytes& b) { if (!b) return nullptr; auto a = std::make_unique<NuahAsset>(); a->bytes = b; return a; } off64_t seek_fail(int e) { errno = e; return -1; }
off64_t seek_asset(NuahAsset* a, off64_t o, int w) { if (!a || !a->bytes) return seek_fail(EINVAL); off64_t b = 0;
  if (w == SEEK_CUR) b = static_cast<off64_t>(a->offset); else if (w == SEEK_END) b = static_cast<off64_t>(a->bytes->size()); else if (w != SEEK_SET) return seek_fail(EINVAL);
  if (o < -b) return seek_fail(EINVAL);
  off64_t d = b + o; if (d < 0 || static_cast<unsigned long long>(d) > a->bytes->size()) return seek_fail(EINVAL); a->offset = static_cast<std::size_t>(d); return d; }
}  // namespace
extern "C" {
void* AAssetManager_fromJava(void*, void*) { std::call_once(manager_once, initialize_manager); return &manager; }
void* AAssetManager_open(void* m, const char* f, int) {
  if (!m || !f || f[0] == '/' || std::strstr(f, "..")) { errno = EINVAL; return nullptr; }  // Path-traversal guard.
  auto* am = static_cast<NuahAssetManager*>(m); std::string key(f);
  { std::unique_lock l(am->mutex); for (;;) { auto c = am->cached.find(key); if (c != am->cached.end()) return make_asset(c->second).release(); if (am->missing.contains(key)) { errno = ENOENT; return nullptr; }
      /* One decompressor per immutable member: ART + Roblox bootstrap can race; without gate both scan APK turning one cold decode into paired stalls. */ if (am->loading.emplace(key).second) break; l.unlock(); std::this_thread::yield(); l.lock(); } }  // Short-lived bootstrap: yield, no second condvar ABI.
  std::string member = std::string("assets/") + key;
  auto publish = [&](AssetBytes b, const char* src) -> void* { if (!b) return nullptr; if (trace_enabled()) std::fprintf(stderr, "nuah assets: opened %s from %s (%zu bytes)\n", key.c_str(), src, b->size()); AssetBytes r = b;
    { std::scoped_lock l(am->mutex); if (b->size() <= kPositiveCacheEntryLimit && am->cached_bytes <= kPositiveCacheLimit - b->size()) { auto [e, ins] = am->cached.emplace(key, b); if (ins) am->cached_bytes += b->size(); else r = e->second; } am->loading.erase(key); } return make_asset(r).release(); };
  for (const auto& root : am->extracted_roots) if (auto b = read_extracted(root, key)) return publish(b, root.c_str());
  for (const auto& apk : am->apks) if (auto b = read_member(apk, member)) return publish(b, apk.c_str());
  if (trace_enabled()) std::fprintf(stderr, "nuah assets: missing %s\n", key.c_str());
  { std::scoped_lock l(am->mutex); am->missing.emplace(key); am->loading.erase(key); } errno = ENOENT; return nullptr; }
#define NUAH_ASSET_LEN(ret, name, expr) ret name(void* a) { [[maybe_unused]] auto* p = static_cast<NuahAsset*>(a); return expr; }
void AAsset_close(void* a) { delete static_cast<NuahAsset*>(a); } const void* AAsset_getBuffer(void* a) { auto* p = static_cast<NuahAsset*>(a); return p && p->bytes && !p->bytes->empty() ? p->bytes->data() : nullptr; }
NUAH_ASSET_LEN(off_t, AAsset_getLength, p && p->bytes ? static_cast<off_t>(p->bytes->size()) : -1) NUAH_ASSET_LEN(off64_t, AAsset_getLength64, static_cast<off64_t>(AAsset_getLength(a)))
int AAsset_read(void* a, void* buf, std::size_t n) { auto* p = static_cast<NuahAsset*>(a); if (!p || (!buf && n)) return static_cast<int>(seek_fail(EINVAL)); if (!p->bytes || p->offset > p->bytes->size()) return static_cast<int>(seek_fail(EINVAL));
  /* Zero-alloc hot path: member decompressed once at open (single-decompressor gate), immutable + ref-counted, so reads are bounded memcpy with no mmap/inflate/lock. */
  auto avail = p->bytes->size() - p->offset, copied = std::min(n, avail); if (copied) { std::memcpy(buf, p->bytes->data() + p->offset, copied); p->offset += copied; } return static_cast<int>(copied); }
off_t AAsset_seek(void* a, off_t o, int w) { return static_cast<off_t>(seek_asset(static_cast<NuahAsset*>(a), o, w)); } off64_t AAsset_seek64(void* a, off64_t o, int w) { return seek_asset(static_cast<NuahAsset*>(a), o, w); }
NUAH_ASSET_LEN(off_t, AAsset_getRemainingLength, p && p->bytes && p->offset <= p->bytes->size() ? static_cast<off_t>(p->bytes->size() - p->offset) : -1) NUAH_ASSET_LEN(off64_t, AAsset_getRemainingLength64, static_cast<off64_t>(AAsset_getRemainingLength(a)))
NUAH_ASSET_LEN(int, AAsset_isAllocated, p && p->bytes ? 1 : 0)
int AAsset_openFileDescriptor(void*, off_t*, off_t*) { /* Android fails for compressed assets. Roblox falls back to AAsset_read/Buffer, which handle stored+deflated via mmap+zlib above. */ errno = ENOTSUP; return -1; } int AAsset_openFileDescriptor64(void*, off64_t*, off64_t*) { return AAsset_openFileDescriptor(nullptr, nullptr, nullptr); }
}  // extern "C"
