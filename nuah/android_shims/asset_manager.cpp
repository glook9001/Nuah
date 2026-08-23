#include "nuah/android_abi_registry.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cerrno>
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

#include <sys/types.h>

namespace {
using AssetBytes = std::shared_ptr<const std::vector<unsigned char>>;

/* AAssetManager is used during bootstrap and can be probed repeatedly by the
 * Android framework.  Keep this bounded: APK assets are immutable for the
 * lifetime of a Nuah process, but an unbounded positive cache would turn a
 * malformed or unusually large APK into permanent RSS growth. */
constexpr std::size_t kPositiveCacheLimit = 64 * 1024 * 1024;
constexpr std::size_t kPositiveCacheEntryLimit = 16 * 1024 * 1024;

struct NuahAssetManager {
  std::vector<std::string> apks;
  std::vector<std::string> extracted_roots;
  std::mutex mutex;
  /* APK members are immutable for the lifetime of a process.  Remember
   * misses so Roblox's repeated optional-probe pattern does not reopen and
   * scan every ZIP on each retry, and retain a bounded set of positive
   * members so repeated framework/texture probes do not decompress the same
   * bytes on the render path.  Remote asset delivery remains Roblox-owned. */
  std::unordered_set<std::string> missing;
  std::unordered_map<std::string, AssetBytes> cached;
  std::unordered_set<std::string> loading;
  std::size_t cached_bytes = 0;
};

struct NuahAsset {
  AssetBytes bytes;
  std::size_t offset = 0;
};

std::once_flag manager_once;
NuahAssetManager manager;

bool trace_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("NUAH_BOOTSTRAP_TRACE");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

void initialize_manager() {
  if (const char* roots = std::getenv("NUAH_ASSET_ROOTS"); roots && *roots) {
    const char* begin = roots;
    for (const char* cursor = roots;; ++cursor) {
      if (*cursor != ':' && *cursor != '\0') continue;
      if (cursor != begin) manager.extracted_roots.emplace_back(begin, cursor);
      if (*cursor == '\0') break;
      begin = cursor + 1;
    }
  }
  if (const char* app_data = std::getenv("ANDROID_APP_DATA_DIR");
      app_data && *app_data) {
    manager.extracted_roots.emplace_back(
        (std::filesystem::path(app_data) / "files/assets").string());
    manager.extracted_roots.emplace_back(
        (std::filesystem::path(app_data) / "assets").string());
  }
  const char* paths = std::getenv("NUAH_APK_PATHS");
  if (!paths) return;
  const char* begin = paths;
  for (const char* cursor = paths;; ++cursor) {
    if (*cursor != ':' && *cursor != '\0') continue;
    if (cursor != begin) manager.apks.emplace_back(begin, cursor);
    if (*cursor == '\0') break;
    begin = cursor + 1;
  }
}

AssetBytes read_member(const std::string& apk, const std::string& member) {
  using ArchivePtr = std::unique_ptr<archive, decltype(&archive_read_free)>;
  ArchivePtr archive_reader(archive_read_new(), archive_read_free);
  if (!archive_reader) return nullptr;
  archive_read_support_filter_all(archive_reader.get());
  archive_read_support_format_zip(archive_reader.get());
  if (archive_read_open_filename(archive_reader.get(), apk.c_str(), 64 * 1024) !=
      ARCHIVE_OK) {
    return nullptr;
  }

  archive_entry* entry = nullptr;
  while (archive_read_next_header(archive_reader.get(), &entry) == ARCHIVE_OK) {
    const char* path = archive_entry_pathname(entry);
    if (!path || member != path) {
      archive_read_data_skip(archive_reader.get());
      continue;
    }
    const auto declared_size = archive_entry_size(entry);
    if (declared_size < 0 || declared_size > 512LL * 1024 * 1024) return nullptr;
    auto bytes = std::make_shared<std::vector<unsigned char>>();
    bytes->resize(static_cast<std::size_t>(declared_size));
    std::size_t done = 0;
    while (done < bytes->size()) {
      const auto count = archive_read_data(
          archive_reader.get(), bytes->data() + done, bytes->size() - done);
      if (count <= 0) return nullptr;
      done += static_cast<std::size_t>(count);
    }
    return std::shared_ptr<const std::vector<unsigned char>>(std::move(bytes));
  }
  return nullptr;
}

AssetBytes read_extracted(const std::string& root, const std::string& member) {
  std::error_code error;
  const auto path = std::filesystem::path(root) / member;
  if (!std::filesystem::is_regular_file(path, error) || error) return nullptr;
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return nullptr;
  const auto end = input.tellg();
  if (end < 0 || end > std::streamoff(512LL * 1024 * 1024)) return nullptr;
  auto bytes = std::make_shared<std::vector<unsigned char>>(
      static_cast<std::size_t>(end));
  input.seekg(0, std::ios::beg);
  if (!bytes->empty() &&
      !input.read(reinterpret_cast<char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()))) {
    return nullptr;
  }
  return std::shared_ptr<const std::vector<unsigned char>>(std::move(bytes));
}

std::unique_ptr<NuahAsset> make_asset(const AssetBytes& bytes) {
  if (!bytes) return nullptr;
  auto asset = std::make_unique<NuahAsset>();
  asset->bytes = bytes;
  return asset;
}

off64_t seek_asset(NuahAsset* asset, off64_t offset, int whence) {
  if (!asset || !asset->bytes) {
    errno = EINVAL;
    return -1;
  }
  off64_t base = 0;
  if (whence == SEEK_CUR) {
    base = static_cast<off64_t>(asset->offset);
  } else if (whence == SEEK_END) {
    base = static_cast<off64_t>(asset->bytes->size());
  } else if (whence != SEEK_SET) {
    errno = EINVAL;
    return -1;
  }
  if (offset < -base) {
    errno = EINVAL;
    return -1;
  }
  const off64_t destination = base + offset;
  if (destination < 0 ||
      static_cast<unsigned long long>(destination) > asset->bytes->size()) {
    errno = EINVAL;
    return -1;
  }
  asset->offset = static_cast<std::size_t>(destination);
  return destination;
}
}  // namespace

extern "C" {
void* AAssetManager_fromJava(void*, void*) {
  std::call_once(manager_once, initialize_manager);
  return &manager;
}

void* AAssetManager_open(void* opaque_manager, const char* filename, int) {
  if (!opaque_manager || !filename || filename[0] == '/' ||
      std::strstr(filename, "..")) {
    errno = EINVAL;
    return nullptr;
  }
  auto* asset_manager = static_cast<NuahAssetManager*>(opaque_manager);
  const std::string cache_key(filename);
  {
    std::unique_lock lock(asset_manager->mutex);
    for (;;) {
      const auto cached = asset_manager->cached.find(cache_key);
      if (cached != asset_manager->cached.end())
        return make_asset(cached->second).release();
      if (asset_manager->missing.contains(cache_key)) {
        errno = ENOENT;
        return nullptr;
      }
      /* Keep one decompressor per immutable member.  AssetManager calls can
       * arrive from ART and the Roblox bootstrap concurrently; without this
       * gate both threads open and scan the APK, which turns one cold decode
       * into a pair of serial render/startup stalls. */
      if (asset_manager->loading.emplace(cache_key).second) break;
      lock.unlock();
      /* This is a short-lived bootstrap path.  Yield instead of holding the
       * manager mutex or adding a second runtime condition-variable ABI. */
      std::this_thread::yield();
      lock.lock();
    }
  }
  const std::string member = std::string("assets/") + cache_key;
  auto publish = [&](AssetBytes bytes, const char* source) -> void* {
    if (!bytes) return nullptr;
    if (trace_enabled()) {
      std::fprintf(stderr, "nuah assets: opened %s from %s (%zu bytes)\n",
                   cache_key.c_str(), source, bytes->size());
    }
    AssetBytes result = bytes;
    {
      std::scoped_lock lock(asset_manager->mutex);
      if (bytes->size() <= kPositiveCacheEntryLimit &&
          asset_manager->cached_bytes <=
              kPositiveCacheLimit - bytes->size()) {
        const auto [entry, inserted] =
            asset_manager->cached.emplace(cache_key, bytes);
        if (inserted) asset_manager->cached_bytes += bytes->size();
        else result = entry->second;
      }
      asset_manager->loading.erase(cache_key);
    }
    return make_asset(result).release();
  };
  for (const auto& root : asset_manager->extracted_roots) {
    if (auto bytes = read_extracted(root, cache_key))
      return publish(bytes, root.c_str());
  }
  for (const auto& apk : asset_manager->apks) {
    if (auto bytes = read_member(apk, member)) {
      return publish(bytes, apk.c_str());
    }
  }
  if (trace_enabled()) {
    std::fprintf(stderr, "nuah assets: missing %s\n", cache_key.c_str());
  }
  {
    std::scoped_lock lock(asset_manager->mutex);
    asset_manager->missing.emplace(cache_key);
    asset_manager->loading.erase(cache_key);
  }
  errno = ENOENT;
  return nullptr;
}

void AAsset_close(void* opaque_asset) {
  delete static_cast<NuahAsset*>(opaque_asset);
}

const void* AAsset_getBuffer(void* opaque_asset) {
  auto* asset = static_cast<NuahAsset*>(opaque_asset);
  return asset && asset->bytes && !asset->bytes->empty()
             ? asset->bytes->data()
             : nullptr;
}

off_t AAsset_getLength(void* opaque_asset) {
  auto* asset = static_cast<NuahAsset*>(opaque_asset);
  return asset && asset->bytes ? static_cast<off_t>(asset->bytes->size()) : -1;
}

off64_t AAsset_getLength64(void* opaque_asset) {
  return static_cast<off64_t>(AAsset_getLength(opaque_asset));
}

int AAsset_read(void* opaque_asset, void* buffer, std::size_t count) {
  auto* asset = static_cast<NuahAsset*>(opaque_asset);
  if (!asset || (!buffer && count)) {
    errno = EINVAL;
    return -1;
  }
  if (!asset->bytes || asset->offset > asset->bytes->size()) {
    errno = EINVAL;
    return -1;
  }
  const auto available = asset->bytes->size() - asset->offset;
  const auto copied = std::min(count, available);
  if (copied) {
    std::memcpy(buffer, asset->bytes->data() + asset->offset, copied);
    asset->offset += copied;
  }
  return static_cast<int>(copied);
}

off_t AAsset_seek(void* opaque_asset, off_t offset, int whence) {
  return static_cast<off_t>(
      seek_asset(static_cast<NuahAsset*>(opaque_asset), offset, whence));
}

off64_t AAsset_seek64(void* opaque_asset, off64_t offset, int whence) {
  return seek_asset(static_cast<NuahAsset*>(opaque_asset), offset, whence);
}

off_t AAsset_getRemainingLength(void* opaque_asset) {
  auto* asset = static_cast<NuahAsset*>(opaque_asset);
  return asset && asset->bytes && asset->offset <= asset->bytes->size()
             ? static_cast<off_t>(asset->bytes->size() - asset->offset)
             : -1;
}

off64_t AAsset_getRemainingLength64(void* opaque_asset) {
  return static_cast<off64_t>(AAsset_getRemainingLength(opaque_asset));
}

int AAsset_isAllocated(void* opaque_asset) {
  auto* asset = static_cast<NuahAsset*>(opaque_asset);
  return asset && asset->bytes ? 1 : 0;
}

int AAsset_openFileDescriptor(void*, off_t*, off_t*) {
  // Android documents failure for compressed assets. Roblox can fall back to
  // AAsset_read/AAsset_getBuffer, which work for both stored and compressed ZIP
  // entries through libarchive.
  errno = ENOTSUP;
  return -1;
}

int AAsset_openFileDescriptor64(void*, off64_t*, off64_t*) {
  errno = ENOTSUP;
  return -1;
}
}  // extern "C"
