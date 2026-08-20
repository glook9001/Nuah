#include "nuah/atl_backend.hpp"
#include "nuah/apk_loader.hpp"
#include "nuah/bootstrap_diagnostics.h"
#include "nuah/input_bridge.h"
#include "nuah/launch_uri.hpp"
#include "nuah/native_session.h"
#include "nuah/nuah_jvm.h"
#include "nuah/window_session.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <signal.h>
#include <unistd.h>
#include <vector>

#include <jni.h>

namespace nuah {
namespace {

extern "C" void nuah_roblox_java_facade_set_content_path(const char* path);
extern "C" void nuah_roblox_java_facade_set_launch_place_id(jlong place_id);
extern "C" void nuah_roblox_java_facade_set_start_game_params(
    const char* access_code, const char* reserved_server_access_code,
    jlong user_id, jint join_request_type);
extern "C" void nuah_roblox_java_facade_set_launch_surface(jobject surface);

using RobloxSetMultipleCookies = void (*)(JNIEnv*, jclass, jstring, jstring);
using RobloxGetCookiesForDomain = jstring (*)(JNIEnv*, jclass, jstring);
RobloxSetMultipleCookies g_roblox_set_multiple_cookies = nullptr;
RobloxGetCookiesForDomain g_roblox_get_cookies_for_domain = nullptr;

void report_bootstrap_stage(const char* stage) {
  nuah_bootstrap_diagnostics_set_stage(stage);
}

/* Keep the SDL/Android input handoff responsive without turning the host
 * thread into a busy spinner.  The old fixed 10 ms sleep made a mouse edge
 * wait almost a full polling interval before it reached Roblox.  Two
 * milliseconds is below a 60 Hz frame and still leaves the Roblox worker
 * threads CPU time; set NUAH_INPUT_POLL_SLEEP_US=0 only for raw-latency
 * diagnostics. */
unsigned int input_poll_sleep_us() {
  static const unsigned int value = [] {
    constexpr unsigned int kDefault = 2000;
    const char* raw = ::getenv("NUAH_INPUT_POLL_SLEEP_US");
    if (!raw || !*raw) return kDefault;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed > 20000UL)
      return kDefault;
    return static_cast<unsigned int>(parsed);
  }();
  return value;
}

/* Keep the launch sequence that produced the known-good 632258f room run as
 * the product path.  The longer setter sequence remains an explicit
 * diagnostic experiment; it is not a safe default for the current Roblox
 * image because it can initialize the render session twice. */
bool fast_mvp_enabled() {
  const char* value = ::getenv("NUAH_FAST_MVP");
  return !value || std::strcmp(value, "0") != 0;
}

void clear_java_exception(JNIEnv* env, const char* boundary) {
  if (!env || !env->ExceptionCheck()) return;
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::cerr << "nuah native: Java exception at " << boundary << '\n';
    env->ExceptionDescribe();
  }
  env->ExceptionClear();
}

/* An Android install may leave an empty Sober-compatible assets/content
 * directory behind while ATL has already unpacked the real APK assets under
 * the app-private files directory.  Existence alone is therefore not a
 * usable asset-root test.  Check for one regular file without walking the
 * whole tree; this keeps startup cheap while rejecting empty placeholders. */
bool asset_tree_has_files(const std::filesystem::path& root) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) return false;
  std::filesystem::recursive_directory_iterator entries(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (entries != end) {
    if (!error && std::filesystem::is_regular_file(entries->path(), error))
      return true;
    error.clear();
    entries.increment(error);
  }
  return false;
}

/* Ask the kernel to pull a bounded amount of already-extracted content into
 * the page cache before Roblox starts its first scene.  This is intentionally
 * only a WILLNEED hint: Nuah does not decode, transcode, or duplicate an
 * Android asset, and remote assets are left to Roblox's AssetProvider.  The
 * bounded pass avoids turning launch into a multi-gigabyte disk scan. */
void prefetch_local_asset_pages(
    const std::array<std::filesystem::path, 2>& roots) {
  const char* enabled = std::getenv("NUAH_ASSET_PREFETCH");
  if (enabled && std::strcmp(enabled, "0") == 0) return;
  unsigned long budget_mb = 512;
  if (const char* raw = std::getenv("NUAH_ASSET_PREFETCH_MB"); raw && *raw) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (end != raw && *end == '\0' && parsed <= 4096) budget_mb = parsed;
  }
  const uint64_t budget = static_cast<uint64_t>(budget_mb) * 1024ULL * 1024ULL;
  uint64_t requested = 0;
  uint64_t files = 0;
  std::error_code error;
  for (const auto& root : roots) {
    if (requested >= budget || root.empty()) break;
    if (!std::filesystem::is_directory(root, error) || error) {
      error.clear();
      continue;
    }
    std::filesystem::recursive_directory_iterator entries(
        root, std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    for (; entries != end && requested < budget; entries.increment(error)) {
      if (error) {
        error.clear();
        continue;
      }
      std::error_code file_error;
      if (!std::filesystem::is_regular_file(entries->path(), file_error) ||
          file_error) continue;
      const uintmax_t size = std::filesystem::file_size(entries->path(),
                                                        file_error);
      if (file_error || size == 0) continue;
      const uint64_t remaining = budget - requested;
      const off_t hint_size = static_cast<off_t>(
          std::min<uint64_t>(remaining, static_cast<uint64_t>(size)));
      const int fd = ::open(entries->path().c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) continue;
      (void)::posix_fadvise(fd, 0, hint_size, POSIX_FADV_WILLNEED);
      (void)::close(fd);
      requested += static_cast<uint64_t>(hint_size);
      ++files;
    }
  }
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace) {
    std::cerr << "nuah assets: prefetch hints files=" << files
              << " bytes=" << requested << " budget=" << budget << '\n';
  }
}

/* The APK carries the same applicationSettings response that the Android
 * client uses for AssetProvider, texture streaming, and scheduler tuning.
 * Those mobile defaults deliberately trade frame cadence for battery and
 * background work; on the desktop bridge they kept the populated room around
 * 30--50 FPS even after the atlas was ready.  Use Nuah's small host settings
 * by default and make the verbatim Android response an explicit compatibility
 * switch.  This keeps the asset implementation Android-owned without forcing
 * its mobile scheduler policy onto the host. */
std::string packaged_client_settings(const std::filesystem::path& app_data) {
  const char* enabled = std::getenv("NUAH_USE_PACKAGED_CLIENT_SETTINGS");
  if (!enabled || !*enabled || std::strcmp(enabled, "0") == 0) {
    return {};
  }
  std::filesystem::path default_nuah_dicts;
  if (const char* home = std::getenv("HOME"); home && *home) {
    default_nuah_dicts = std::filesystem::path(home) /
                         ".local/share/nuah/base.apk_/files/assets/"
                         "shared_compression_dictionaries";
  }
  const std::array<std::filesystem::path, 3> roots = {
      app_data / "files/assets/shared_compression_dictionaries",
      app_data / "assets/shared_compression_dictionaries",
      default_nuah_dicts};
  for (const auto& root : roots) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) continue;
    for (const auto& entry : std::filesystem::directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             error)) {
      if (error) break;
      if (!entry.is_regular_file(error) || error) {
        error.clear();
        continue;
      }
      /* The directory also contains zstd dictionaries.  Some dictionaries
       * happen to contain the marker text, but passing their binary bytes as
       * applicationSettings makes Roblox dereference an invalid settings
       * object during StartGameWithParam.  Only accept an actual JSON file
       * whose first non-space byte is an object. */
      if (entry.path().extension() != ".json") continue;
      std::ifstream input(entry.path(), std::ios::binary | std::ios::ate);
      if (!input) continue;
      const auto end = input.tellg();
      if (end <= 0 || end > std::streamoff(8 * 1024 * 1024)) continue;
      std::string contents(static_cast<std::size_t>(end), '\0');
      input.seekg(0, std::ios::beg);
      if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())))
        continue;
      const std::size_t first = contents.find_first_not_of(" \t\r\n");
      if (first == std::string::npos || contents[first] != '{') continue;
      if (contents.find("\"applicationSettings\"") == std::string::npos)
        continue;
      return contents;
    }
  }
  return {};
}

/* Replace one scalar in the compact applicationSettings object without
 * pulling a second JSON library into the native runtime. Values used here are
 * either JSON booleans or quoted decimal strings. */
void set_client_setting(std::string& json, std::string_view key,
                        std::string_view value) {
  const std::string needle = "\"" + std::string(key) + "\"";
  std::size_t key_pos = json.find(needle);
  if (key_pos != std::string::npos) {
    const std::size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string::npos) return;
    std::size_t begin = colon + 1;
    while (begin < json.size() &&
           (json[begin] == ' ' || json[begin] == '\t' ||
            json[begin] == '\r' || json[begin] == '\n')) {
      ++begin;
    }
    std::size_t end = begin;
    if (begin < json.size() && json[begin] == '"') {
      ++end;
      bool escaped = false;
      for (; end < json.size(); ++end) {
        if (!escaped && json[end] == '"') {
          ++end;
          break;
        }
        escaped = !escaped && json[end] == '\\';
        if (json[end] != '\\') escaped = false;
      }
    } else {
      while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
      while (end > begin &&
             (json[end - 1] == ' ' || json[end - 1] == '\t' ||
              json[end - 1] == '\r' || json[end - 1] == '\n')) {
        --end;
      }
    }
    json.replace(begin, end - begin, value);
    return;
  }
  const std::size_t outer_end = json.rfind('}');
  if (outer_end == std::string::npos || outer_end == 0) return;
  const std::size_t settings_end = json.rfind('}', outer_end - 1);
  if (settings_end == std::string::npos) return;
  json.insert(settings_end, "," + needle + ":" + std::string(value));
}

void configure_mesa_shader_cache(const std::filesystem::path& profile) {
  const char* mode = ::getenv("NUAH_SHADER_CACHE");
  if (mode && std::strcmp(mode, "0") == 0) return;

  /* Keep the persistent profile cache as the safe default, but allow a
   * caller to place the same Mesa cache on tmpfs.  On this host the cold
   * Intel pipeline lookup can spend seconds in disk_cache_load_item while
   * FunctionMarshal is synchronously creating a graphics pipeline.  The
   * override is deliberately just a directory selection: Mesa still owns
   * cache format, locking, invalidation, and shader compilation. */
  std::filesystem::path directory = profile / "mesa-shader-cache";
  if (const char* requested = ::getenv("NUAH_SHADER_CACHE_DIR");
      requested && *requested) {
    directory = requested;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::cerr << "nuah graphics: shader cache unavailable: "
                << error.message() << '\n';
    }
    return;
  }
  if (!std::getenv("MESA_SHADER_CACHE_DIR"))
    (void)::setenv("MESA_SHADER_CACHE_DIR", directory.c_str(), 1);
  if (!std::getenv("MESA_SHADER_CACHE_MAX_SIZE")) {
    const char* requested = std::getenv("NUAH_SHADER_CACHE_MAX_SIZE");
    (void)::setenv("MESA_SHADER_CACHE_MAX_SIZE",
                   requested && *requested ? requested : "1G", 1);
  }
  if (!std::getenv("MESA_SHADER_CACHE_DISABLE"))
    (void)::setenv("MESA_SHADER_CACHE_DISABLE", "false", 1);
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::cerr << "nuah graphics: Mesa shader cache=" << directory << '\n';
}

/* The governor deliberately has only two bounded profiles.  It is not an
 * engine patch: explicit NUAH_TASK_THREADS, NUAH_ASSET_PROVIDER_THREADS,
 * NUAH_RENDER_TEXTURE_BUDGET_MS, and NUAH_VULKAN_SUBMIT_THREAD values always
 * win.  "balanced" leaves more CPU time for FunctionMarshal; "throughput"
 * preserves the tuned streaming profile.  Both retain Mesa's stable submit
 * thread; disabling it is an independent diagnostic, not a governor action. */
const char* engine_governor_profile() {
  const char* value = ::getenv("NUAH_ENGINE_GOVERNOR");
  if (!value || !*value || std::strcmp(value, "0") == 0 ||
      std::strcmp(value, "off") == 0)
    return nullptr;
  if (std::strcmp(value, "balanced") == 0 ||
      std::strcmp(value, "throughput") == 0)
    return value;
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::cerr << "nuah governor: unknown profile '" << value
              << "' (use balanced, throughput, or off)\n";
  return nullptr;
}

/* ANV can submit command buffers on a dedicated driver thread. On the
 * measured four-thread Intel host, keeping submission on FunctionMarshal
 * uses less CPU at the same 60-Hz FIFO cadence (8.34 vs 9.42 CPU-seconds in
 * matched 15-second runs). Mesa ignores this variable on other Vulkan
 * implementations. Keep an explicit 0/1 override and never overwrite a
 * caller-provided Mesa setting. */
void configure_mesa_submit_thread() {
  const char* requested = ::getenv("NUAH_VULKAN_SUBMIT_THREAD");
  const char* existing = ::getenv("MESA_VK_ENABLE_SUBMIT_THREAD");
  if (existing && (!requested || std::strcmp(requested, "auto") == 0)) return;

  const char* value = nullptr;
  if (requested && *requested && std::strcmp(requested, "auto") != 0) {
    if (std::strcmp(requested, "0") != 0 && std::strcmp(requested, "1") != 0) {
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::cerr << "nuah graphics: invalid NUAH_VULKAN_SUBMIT_THREAD='"
                  << requested << "' (use auto, 0, or 1)\n";
      }
      return;
    }
    value = requested;
  } else {
    if (engine_governor_profile()) value = "0";
    if (value) {
      if (::setenv("MESA_VK_ENABLE_SUBMIT_THREAD", value, 1) != 0) return;
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
        std::cerr << "nuah graphics: Mesa submit thread="
                  << (std::strcmp(value, "1") == 0 ? "enabled" : "disabled")
                  << '\n';
      return;
    }
    const unsigned int logical_cpus = std::thread::hardware_concurrency();
    if (logical_cpus == 0 || logical_cpus > 4) return;
    value = "0";
  }
  if (::setenv("MESA_VK_ENABLE_SUBMIT_THREAD", value, 1) != 0) return;
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::cerr << "nuah graphics: Mesa submit thread="
              << (std::strcmp(value, "1") == 0 ? "enabled" : "disabled")
              << '\n';
}

/* The current x86_64 Roblox image's implementation is a six-byte function
 * that returns the constant 3.  libhybris does not expose that versioned
 * symbol through android_dlsym, so register the observed contract directly
 * instead of adding a general native-method emulator. */
extern "C" jint nuah_native_get_running_architecture(JNIEnv*, jclass) {
  return 3;
}

/* Cookie persistence belongs to the WebKit/session supervisor.  A Roblox
 * Android build asks for two different representations: the engine bootstrap
 * parses a Netscape cookie file, while the HTTP cookie bridge wants a normal
 * `name=value` header.  Returning one representation for both calls makes the
 * bootstrap report "Invalid cookie format" and silently drops authentication.
 * Keep the source deliberately narrow: NUAH_ROBLOX_COOKIES contains only the
 * .ROBLOSECURITY value (or the same value prefixed with its cookie name). */
std::string nuah_roblox_cookie_value() {
  const char* raw = ::getenv("NUAH_ROBLOX_COOKIES");
  if (!raw || !*raw) return {};
  std::string value(raw);
  constexpr std::string_view prefix = ".ROBLOSECURITY=";
  if (value.starts_with(prefix)) value.erase(0, prefix.size());
  return value;
}

std::string nuah_roblox_cookie_header() {
  if (const char* header = ::getenv("NUAH_ROBLOX_COOKIE_HEADER");
      header && *header) {
    return header;
  }
  const std::string value = nuah_roblox_cookie_value();
  return value.empty() ? std::string() : ".ROBLOSECURITY=" + value;
}

jlong nuah_roblox_user_id() {
  const char* raw = ::getenv("NUAH_ROBLOX_USER_ID");
  if (!raw || !*raw) return 0;
  char* end = nullptr;
  const long long value = std::strtoll(raw, &end, 10);
  if (end == raw || *end != '\0' || value <= 0) return 0;
  return static_cast<jlong>(value);
}

std::string jstring_utf8(JNIEnv* env, jstring value) {
  if (!env || !value) return {};
  const char* raw = env->GetStringUTFChars(value, nullptr);
  if (!raw) {
    clear_java_exception(env, "GetStringUTFChars(cookie)");
    return {};
  }
  std::string result(raw);
  env->ReleaseStringUTFChars(value, raw);
  return result;
}

void extract_cookie_user_id_and_name(const std::string& line,
                                       const std::string& sec_value);

void adopt_cookie_header(std::string_view header) {
  constexpr std::string_view marker = ".ROBLOSECURITY=";
  const std::size_t marker_pos = header.find(marker);
  if (marker_pos == std::string_view::npos) return;
  const std::size_t value_begin = marker_pos + marker.size();
  const std::size_t value_end = header.find_first_of(";\t\r\n", value_begin);
  const std::string_view value = header.substr(
      value_begin, value_end == std::string_view::npos
                      ? std::string_view::npos
                      : value_end - value_begin);
  if (value.empty() || value.size() > 4096 ||
      value.find_first_of("\t\r\n") != std::string_view::npos) {
    return;
  }
  const std::string cookie = std::string(marker) + std::string(value);
  (void)::setenv("NUAH_ROBLOX_COOKIES", cookie.c_str(), 1);
  if (header.size() <= 16384 &&
      header.find_first_of("\r\n") == std::string_view::npos) {
    const std::string full(header);
    (void)::setenv("NUAH_ROBLOX_COOKIE_HEADER", full.c_str(), 1);
  }
  extract_cookie_user_id_and_name(std::string(header), std::string(value));
}

void prime_roblox_cookie_store(JNIEnv* env, bool early_bootstrap) {
  /* The first setter call happens before MainGameActivity has finished
   * creating Roblox's native cookie store and is unsafe on this APK.  Keep
   * this call disabled by default.  The second call, immediately before
   * StartGameWithParam, is the authenticated launch handoff and must still
   * run or a fresh profile can sit forever at UGCGame without ever reaching
   * the renderer.  Set NUAH_EARLY_COOKIE_PRIME=1 only for an old-runtime
   * comparison; NUAH_SKIP_COOKIE_PRIME remains a compatible spelling. */
  if (early_bootstrap) {
    const char* force = ::getenv("NUAH_EARLY_COOKIE_PRIME");
    const bool force_enabled =
        force && *force && std::strcmp(force, "0") != 0;
    const char* skip = ::getenv("NUAH_SKIP_COOKIE_PRIME");
    const bool skip_enabled = skip && *skip && std::strcmp(skip, "0") != 0;
    if (!force_enabled || skip_enabled) {
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace)
        std::fprintf(stderr,
                     "nuah cookie: skipping early native-store prime\n");
      return;
    }
  }
  if (!env || !g_roblox_set_multiple_cookies) return;
  const std::string cookie = nuah_roblox_cookie_header();
  if (cookie.empty()) return;
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::fprintf(stderr, "nuah cookie: native setter=%p header_bytes=%zu\n",
                 reinterpret_cast<void*>(g_roblox_set_multiple_cookies),
                 cookie.size());
  }
  jstring url = env->NewStringUTF("https://roblox.com/");
  jstring header = env->NewStringUTF(cookie.c_str());
  if (!url || !header) {
    clear_java_exception(env, "NewStringUTF(cookie prime)");
    if (url) env->DeleteLocalRef(url);
    if (header) env->DeleteLocalRef(header);
    return;
  }
  g_roblox_set_multiple_cookies(env, nullptr, url, header);
  if (env->ExceptionCheck()) clear_java_exception(env, "nativeSetMultipleCookies(prime)");
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace &&
      g_roblox_get_cookies_for_domain) {
    jstring stored = g_roblox_get_cookies_for_domain(env, nullptr, url);
    const std::string stored_header = jstring_utf8(env, stored);
    if (stored) env->DeleteLocalRef(stored);
    std::cerr << "nuah cookie: Roblox native store header_bytes="
              << stored_header.size() << '\n';
  }
  env->DeleteLocalRef(url);
  env->DeleteLocalRef(header);
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::cerr << "nuah cookie: primed native store header_bytes=" << cookie.size()
              << '\n';
  }
}

void extract_cookie_user_id_and_name(const std::string& line,
                                       const std::string& sec_value) {
  for (std::string_view marker :
       {"rbxuid=", "UserID=", "userid="}) {
    const std::size_t pos = line.find(marker);
    if (pos != std::string::npos) {
      const std::size_t begin = pos + marker.size();
      const std::size_t end = line.find_first_not_of("0123456789", begin);
      const std::string id = line.substr(
          begin, end == std::string::npos ? std::string::npos : end - begin);
      if (!id.empty() && id.size() <= 20) {
        (void)::setenv("NUAH_ROBLOX_USER_ID", id.c_str(), 1);
        break;
      }
    }
  }
  const std::size_t prefix_pos = sec_value.find("|_");
  if (prefix_pos != std::string::npos) {
    const std::size_t b64_start = prefix_pos + 2;
    const std::size_t b64_end = sec_value.find('.', b64_start);
    const std::string b64 = sec_value.substr(
        b64_start, b64_end == std::string::npos ? std::string::npos
                                                : b64_end - b64_start);
    static const int b64_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    std::string decoded;
    int val = 0, valb = -8;
    for (unsigned char c : b64) {
      if (b64_table[c] == -1) break;
      val = (val << 6) + b64_table[c];
      valb += 6;
      if (valb >= 0) {
        decoded.push_back(char((val >> valb) & 0xFF));
        valb -= 8;
      }
    }
    const std::size_t uid_pos = decoded.find("\x03uid\x12");
    if (uid_pos != std::string::npos) {
      std::size_t dig_start = uid_pos + 5;
      if (dig_start < decoded.size() &&
          static_cast<unsigned char>(decoded[dig_start]) < 32) {
        ++dig_start;
      }
      const std::size_t dig_end =
          decoded.find_first_not_of("0123456789", dig_start);
      const std::string uid = decoded.substr(
          dig_start, dig_end == std::string::npos ? std::string::npos
                                                  : dig_end - dig_start);
      if (!uid.empty() && uid.size() <= 20) {
        (void)::setenv("NUAH_ROBLOX_USER_ID", uid.c_str(), 1);
      }
    }
    const std::size_t uname_pos = decoded.find("\x05uname\x12");
    if (uname_pos != std::string::npos) {
      std::size_t u_start = uname_pos + 7;
      if (u_start < decoded.size() &&
          static_cast<unsigned char>(decoded[u_start]) < 32) {
        ++u_start;
      }
      const std::size_t u_end = decoded.find_first_of(
          "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
          "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
          "\"\t\r\n; ",
          u_start);
      const std::string uname = decoded.substr(
          u_start, u_end == std::string::npos ? std::string::npos
                                              : u_end - u_start);
      if (!uname.empty() && uname.size() <= 50) {
        (void)::setenv("NUAH_ROBLOX_USERNAME", uname.c_str(), 1);
      }
    }
  }
}

/* Direct native-run has no WebKit supervisor to press the explicit
 * "use-browser-session" button.  Sober already persists the live session in
 * a mode-600 one-line cookie file, so adopt that file at the process boundary
 * when the caller did not supply a session explicitly.  This keeps the
 * runtime launchable from the normal Nuah command as well as from Services;
 * the token never enters argv or a log line. */
bool discover_sober_session_cookie() {
  if (const char* existing = ::getenv("NUAH_ROBLOX_COOKIES");
      existing && *existing) {
    return true;
  }
  std::vector<std::filesystem::path> candidates;
  if (const char* configured = ::getenv("NUAH_SOBER_COOKIE_FILE");
      configured && *configured) {
    candidates.emplace_back(configured);
  }
  if (const char* nuah_data = ::getenv("NUAH_DATA_DIR"); nuah_data && *nuah_data) {
    candidates.push_back(std::filesystem::path(nuah_data) / "cookies");
  }
  if (const char* home = ::getenv("HOME"); home && *home) {
    const std::filesystem::path home_path(home);
    candidates.push_back(home_path / ".local/share/nuah/cookies");
    candidates.push_back(home_path / ".var/app/org.vinegarhq.Sober/data/sober/cookies");
    candidates.push_back(home_path / ".config/sober/cookies");
  }
  for (const auto& path : candidates) {
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) continue;
    std::ifstream file(path, std::ios::in | std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
      /* Sober stores one combined Cookie header (GuestData; ...;
       * .ROBLOSECURITY=token), rather than a Netscape table.  Accept that
       * format as well as a line containing only the Roblox cookie. */
      constexpr std::string_view marker = ".ROBLOSECURITY=";
      const std::size_t marker_pos = line.find(marker);
      if (marker_pos == std::string::npos) continue;
      const std::size_t value_begin = marker_pos + marker.size();
      const std::size_t value_end = line.find_first_of(";\t\r\n", value_begin);
      const std::string value = line.substr(
          value_begin, value_end == std::string::npos ? std::string::npos
                                                        : value_end - value_begin);
      if (value.empty() || value.find_first_of("\r\n\t") !=
                               std::string::npos || value.size() > 4096) {
        continue;
      }
      const std::string cookie = ".ROBLOSECURITY=" + value;
      if (::setenv("NUAH_ROBLOX_COOKIES", cookie.c_str(), 1) != 0) {
        throw std::runtime_error("cannot import Sober session cookie");
      }
      if (line.size() <= 16384 &&
          line.find_first_of("\r\n") == std::string::npos) {
        (void)::setenv("NUAH_ROBLOX_COOKIE_HEADER", line.c_str(), 1);
      }
      extract_cookie_user_id_and_name(line, value);
      std::cerr << "nuah native: adopted Sober browser session from "
                << path << '\n';
      return true;
    }
  }
  return false;
}

/* Sober's one-line export intentionally keeps only the authentication cookie;
 * the WebKit SQLite jar also contains RBXEventTrackerV2, whose rbxuid field
 * is the account id the Android launch JSON normally supplies.  Recover that
 * non-secret numeric field locally so a bare roblox://placeId URI follows the
 * same authenticated request-type path as Sober. */
bool discover_webkit_user_id() {
  if (nuah_roblox_user_id() > 0) return true;
  std::vector<std::filesystem::path> candidates;
  if (const char* configured = ::getenv("NUAH_WEBKIT_COOKIE_DATABASE");
      configured && *configured) {
    candidates.emplace_back(configured);
  }
  if (const char* home = ::getenv("HOME"); home && *home) {
    candidates.emplace_back(std::filesystem::path(home) /
                            ".local/share/nuah/webkit/cookies.sqlite");
  }
  for (const auto& path : candidates) {
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    const auto size = status_error || !std::filesystem::is_regular_file(status)
                          ? 0
                          : std::filesystem::file_size(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        size > 4 * 1024 * 1024) {
      continue;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) continue;
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    constexpr std::string_view marker = "rbxuid=";
    const std::size_t marker_pos = bytes.find(marker);
    if (marker_pos == std::string::npos) continue;
    const std::size_t begin = marker_pos + marker.size();
    std::size_t end = begin;
    while (end < bytes.size() && bytes[end] >= '0' && bytes[end] <= '9' &&
           end - begin <= 20) {
      ++end;
    }
    if (end == begin || end - begin > 20) continue;
    const std::string user_id = bytes.substr(begin, end - begin);
    char* parse_end = nullptr;
    const long long parsed = std::strtoll(user_id.c_str(), &parse_end, 10);
    if (!parse_end || *parse_end != '\0' || parsed <= 0) continue;
    (void)::setenv("NUAH_ROBLOX_USER_ID", user_id.c_str(), 1);
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::cerr << "nuah native: adopted Roblox user id from WebKit session\n";
    }
    return true;
  }
  return false;
}

extern "C" jstring nuah_native_get_cookies_netscape(JNIEnv* env, jclass,
                                                      jstring) {
  const std::string header = nuah_roblox_cookie_header();
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::cerr << "nuah cookie: Netscape getter header_bytes=" << header.size()
              << '\n';
  }
  if (!env || header.empty() ||
      header.find_first_of("\r\n") != std::string::npos) {
    return env ? env->NewStringUTF("") : nullptr;
  }
  /* The APK's updateCookiesFromEngine parser splits this return value on
   * semicolons, then expects one tab-delimited Netscape record.  Keep the
   * record self-contained (no comment/newline) so it survives that Android
   * boundary and is copied into the engine cookie store. */
  std::string netscape;
  std::size_t offset = 0;
  while (offset < header.size()) {
    const std::size_t next = header.find(';', offset);
    std::string_view pair(
        header.data() + offset,
        (next == std::string::npos ? header.size() : next) - offset);
    while (!pair.empty() && (pair.front() == ' ' || pair.front() == '\t')) {
      pair.remove_prefix(1);
    }
    const std::size_t equal = pair.find('=');
    if (equal != std::string_view::npos && equal > 0) {
      const std::string_view name = pair.substr(0, equal);
      const std::string_view value = pair.substr(equal + 1);
      if (!value.empty() &&
          value.find_first_of("\r\n\t") == std::string_view::npos) {
        if (!netscape.empty()) netscape.push_back(';');
        netscape += "roblox.com\tTRUE\t/\tTRUE\t0\t";
        netscape.append(name.data(), name.size());
        netscape.push_back('\t');
        netscape.append(value.data(), value.size());
      }
    }
    if (next == std::string::npos) break;
    offset = next + 1;
  }
  return env->NewStringUTF(netscape.c_str());
}

extern "C" jstring nuah_native_get_cookies_for_domain(JNIEnv* env, jclass,
                                                        jstring) {
  const std::string header = nuah_roblox_cookie_header();
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::cerr << "nuah cookie: domain getter header_bytes=" << header.size()
              << '\n';
  }
  return env ? env->NewStringUTF(header.c_str()) : nullptr;
}

extern "C" void nuah_native_set_cookies(JNIEnv* env, jclass, jstring domain,
                                          jstring cookies) {
  const std::string domain_value = jstring_utf8(env, domain);
  const std::string cookie_value = jstring_utf8(env, cookies);
  adopt_cookie_header(cookie_value);
  /* Keep Roblox's own native cookie store in the path.  Nuah only supplies
   * the session boundary; the exported implementation owns the HTTP client
   * state used by gamejoin.roblox.com. */
  /* Sober's migration callback can be empty even when its live browser
   * session was adopted at process start.  Do not let that empty callback
   * erase the already-primed native cookie store. */
  const bool preserve_primed_session =
      cookie_value.empty() && !nuah_roblox_cookie_header().empty();
  if (g_roblox_set_multiple_cookies && !preserve_primed_session) {
    g_roblox_set_multiple_cookies(env, nullptr, domain, cookies);
  }
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::cerr << "nuah cookie: setter domain_bytes=" << domain_value.size()
              << " header_bytes=" << cookie_value.size()
              << " adopted="
              << (nuah_roblox_cookie_value().empty() ? "no" : "yes")
              << " forwarded=" << (preserve_primed_session ? "no" : "yes")
              << '\n';
  }
}

/* InitParams is an AutoValue contract, not a plain data blob.  AllocObject()
 * leaves all final fields null and is exactly what caused the V2 path to
 * abort in GetObjectClass.  Build the object through the app's own builder so
 * ART and Roblox see the same typed object Android would construct. */
jobject make_real_init_params(JNIEnv* env, jobject activity) {
  if (!env) return nullptr;
  constexpr const char* kBuilderClass =
      "com/roblox/engine/jni/autovalue/InitParams$Builder";
  constexpr const char* kBuilderReturn =
      "Lcom/roblox/engine/jni/autovalue/InitParams$Builder;";
  const auto builder_signature = [&](const char* prefix) {
    return std::string(prefix) + kBuilderReturn;
  };
  jclass init_class = env->FindClass("com/roblox/engine/jni/autovalue/InitParams");
  jclass builder_class = env->FindClass(kBuilderClass);
  jclass platform_class =
      env->FindClass("com/roblox/engine/jni/model/PlatformParams");
  jclass device_class =
      env->FindClass("com/roblox/engine/jni/model/DeviceParams");
  if (!init_class || !builder_class || !platform_class || !device_class) {
    clear_java_exception(env, "InitParams classes");
    return nullptr;
  }
  const jmethodID builder_method = env->GetStaticMethodID(
      init_class, "builder", "()Lcom/roblox/engine/jni/autovalue/InitParams$Builder;");
  const jmethodID platform_ctor =
      env->GetMethodID(platform_class, "<init>", "()V");
  const jmethodID device_ctor = env->GetMethodID(device_class, "<init>", "()V");
  if (!builder_method || !platform_ctor || !device_ctor) {
    clear_java_exception(env, "InitParams constructors");
    return nullptr;
  }
  jobject builder = env->CallStaticObjectMethod(init_class, builder_method);
  jobject platform = env->NewObject(platform_class, platform_ctor);
  jobject device = env->NewObject(device_class, device_ctor);
  if (!builder || !platform || !device || env->ExceptionCheck()) {
    clear_java_exception(env, "InitParams builder allocation");
    return nullptr;
  }

  auto set_string_field = [&](jobject object, jclass klass, const char* name,
                              const char* value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "Ljava/lang/String;");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    const jstring text = env->NewStringUTF(value ? value : "");
    env->SetObjectField(object, field, text);
    env->DeleteLocalRef(text);
    return !env->ExceptionCheck();
  };
  auto set_bool_field = [&](jobject object, jclass klass, const char* name,
                            jboolean value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "Z");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    env->SetBooleanField(object, field, value);
    return !env->ExceptionCheck();
  };
  auto set_int_field = [&](jobject object, jclass klass, const char* name,
                           jint value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "I");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    env->SetIntField(object, field, value);
    return !env->ExceptionCheck();
  };
  auto set_long_field = [&](jobject object, jclass klass, const char* name,
                            jlong value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "J");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    env->SetLongField(object, field, value);
    return !env->ExceptionCheck();
  };
  auto set_float_field = [&](jobject object, jclass klass, const char* name,
                             jfloat value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "F");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    env->SetFloatField(object, field, value);
    return !env->ExceptionCheck();
  };

  const char* content = ::getenv("NUAH_CONTENT_PATH");
  if (!content || !*content) content = "";
  const bool fields_ok =
      set_string_field(platform, platform_class, "assetFolderPath", content) &&
      set_float_field(platform, platform_class, "dpiScale", 1.0f) &&
      set_bool_field(platform, platform_class, "isKeyboardDevice", JNI_TRUE) &&
      set_bool_field(platform, platform_class, "isMouseDevice", JNI_TRUE) &&
      set_bool_field(platform, platform_class, "isTouchDevice", JNI_TRUE) &&
      set_int_field(platform, platform_class, "viewportHeightMm", 190) &&
      set_int_field(platform, platform_class, "viewportWidthMm", 340) &&
      set_string_field(device, device_class, "appBuildVariant", "release") &&
      set_string_field(device, device_class, "appVersion", "Roblox") &&
      set_string_field(device, device_class, "country", "US") &&
      set_bool_field(device, device_class, "cpu64Bit", JNI_TRUE) &&
      set_string_field(device, device_class, "deviceName", "Nuah Linux PC") &&
      set_string_field(device, device_class, "deviceSku", "x86_64") &&
      set_int_field(device, device_class, "deviceTotalMemoryMB", 4096) &&
      set_int_field(device, device_class, "displayPhysicalHeightPixels", 720) &&
      set_int_field(device, device_class, "displayPhysicalWidthPixels", 1280) &&
      set_string_field(device, device_class, "displayResolution", "1280x720") &&
      set_bool_field(device, device_class, "isChrome", JNI_FALSE) &&
      set_bool_field(device, device_class, "isLowRamDevice", JNI_FALSE) &&
      set_int_field(device, device_class, "largeMemoryClass", 512) &&
      set_long_field(device, device_class,
                     "lowMemoryKillerBackgroundAppThreshold", 0) &&
      set_long_field(device, device_class,
                     "lowMemoryKillerForegroundAppThreshold", 0) &&
      set_string_field(device, device_class, "manufacturer", "Nuah") &&
      set_int_field(device, device_class, "memoryClass", 256) &&
      set_string_field(device, device_class, "networkType", "WIFI") &&
      set_string_field(device, device_class, "osVersion", "36") &&
      set_string_field(device, device_class, "socModel", "x86_64") &&
      set_string_field(device, device_class, "testDeviceName", "");
  if (!fields_ok || env->ExceptionCheck()) {
    clear_java_exception(env, "InitParams field population");
    return nullptr;
  }

  auto set_object = [&](const char* name, const char* signature,
                        jobject value) -> bool {
    const jmethodID method = env->GetMethodID(builder_class, name, signature);
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };
  auto set_string = [&](const char* name, const char* value) -> bool {
    const jstring text = env->NewStringUTF(value);
    const std::string signature = builder_signature("(Ljava/lang/String;)");
    const bool result = set_object(name, signature.c_str(), text);
    env->DeleteLocalRef(text);
    return result;
  };
  auto set_bool = [&](const char* name, jboolean value) -> bool {
    const std::string signature = builder_signature("(Z)");
    const jmethodID method =
        env->GetMethodID(builder_class, name, signature.c_str());
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };

  if (!set_object("setPlatformParams",
                  builder_signature(
                      "(Lcom/roblox/engine/jni/model/PlatformParams;)")
                      .c_str(),
                  platform) ||
      !set_object("setDeviceParams",
                  builder_signature(
                      "(Lcom/roblox/engine/jni/model/DeviceParams;)")
                      .c_str(),
                  device) ||
      !set_string("setBaseURL", "https://www.roblox.com/") ||
      !set_string("setUserAgent", "Roblox/Android Nuah") ||
      !set_bool("setIsPotato", JNI_FALSE) ||
      !set_bool("setIsTablet", JNI_FALSE) ||
      !set_bool("setIsVrDevice", JNI_FALSE) ||
      !set_string("setBuildVariant", "release") ||
      !set_object("setVrContext",
                  builder_signature("(Landroid/app/Activity;)").c_str(),
                  activity)) {
    return nullptr;
  }
  const jmethodID build = env->GetMethodID(
      builder_class, "build", "()Lcom/roblox/engine/jni/autovalue/InitParams;");
  if (!build) {
    clear_java_exception(env, "InitParams.build");
    return nullptr;
  }
  jobject result = env->CallObjectMethod(builder, build);
  if (!result || env->ExceptionCheck()) {
    clear_java_exception(env, "InitParams.build");
    return nullptr;
  }
  return result;
}

/* StartGameParams is another AutoValue contract.  AllocObject() is not a
 * valid substitute here: every final field remains null and Roblox's native
 * entry point immediately dereferences deviceParams/platformParams (ART
 * aborts with "java_object == null").  Use the APK's own builder so this
 * direct native launch has the same typed object that MainGameActivity would
 * pass. */
jobject make_real_start_game_params(JNIEnv* env, jobject surface,
                                    jobject activity, const LaunchRequest& request,
                                    const char* content_path) {
  if (!env || !surface || !activity) return nullptr;
  jclass params_class =
      env->FindClass("com/roblox/engine/jni/autovalue/StartGameParams");
  jclass builder_class =
      env->FindClass("com/roblox/engine/jni/autovalue/StartGameParams$Builder");
  jclass platform_class =
      env->FindClass("com/roblox/engine/jni/model/PlatformParams");
  jclass device_class =
      env->FindClass("com/roblox/engine/jni/model/DeviceParams");
  if (!params_class || !builder_class || !platform_class || !device_class) {
    clear_java_exception(env, "StartGameParams classes");
    return nullptr;
  }
  /* The APK used by the known-good 632258f launch wraps PlatformParams in
   * ml/a and carries the tablet bit on that concrete object.  Newer APKs may
   * omit or repurpose the obfuscated class, so keep the base model as the
   * safe default and make the old contract an explicit A/B switch. */
  bool use_platform_subclass = false;
  if (const char* value = std::getenv("NUAH_START_PLATFORM_SUBCLASS");
      value && *value && std::strcmp(value, "0") != 0) {
    use_platform_subclass = true;
  }
  jclass game_platform_class = nullptr;
  jmethodID game_platform_ctor = nullptr;
  if (use_platform_subclass) {
    game_platform_class = env->FindClass("ml/a");
    if (game_platform_class) {
      game_platform_ctor = env->GetMethodID(
          game_platform_class, "<init>",
          "(Lcom/roblox/engine/jni/model/PlatformParams;)V");
    }
    if (!game_platform_class || !game_platform_ctor) {
      clear_java_exception(env, "StartGameParams platform subclass");
      use_platform_subclass = false;
      game_platform_class = nullptr;
      game_platform_ctor = nullptr;
    }
  }
  const jmethodID builder_method = env->GetStaticMethodID(
      params_class, "builder",
      "()Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;");
  const jmethodID platform_ctor =
      env->GetMethodID(platform_class, "<init>", "()V");
  if (!builder_method || !platform_ctor) {
    clear_java_exception(env, "StartGameParams constructors");
    return nullptr;
  }
  jobject builder = env->CallStaticObjectMethod(params_class, builder_method);
  jobject base_platform = env->NewObject(platform_class, platform_ctor);
  jobject platform = base_platform;
  if (use_platform_subclass && base_platform) {
    platform = env->NewObject(game_platform_class, game_platform_ctor,
                              base_platform);
    if (platform) {
      const jfieldID tablet =
          env->GetFieldID(game_platform_class, "isTablet", "Z");
      if (tablet) env->SetBooleanField(platform, tablet, JNI_FALSE);
      if (env->ExceptionCheck()) {
        clear_java_exception(env, "StartGameParams platform tablet flag");
        return nullptr;
      }
    }
  }
  if (!builder || !platform || env->ExceptionCheck()) {
    clear_java_exception(env, "StartGameParams allocation");
    return nullptr;
  }

  const char* content = content_path ? content_path : "";
  auto call_object = [&](const char* name, const char* signature,
                         jobject value) -> bool {
    const jmethodID method = env->GetMethodID(builder_class, name, signature);
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };
  auto call_string = [&](const char* name, const char* value) -> bool {
    jstring text = env->NewStringUTF(value ? value : "");
    if (!text) return false;
    const bool ok = call_object(name, "(Ljava/lang/String;)"
                                       "Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;",
                                text);
    env->DeleteLocalRef(text);
    return ok;
  };
  auto call_long = [&](const char* name, jlong value) -> bool {
    const jmethodID method = env->GetMethodID(
        builder_class, name,
        "(J)Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;");
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };
  auto call_int = [&](const char* name, jint value) -> bool {
    const jmethodID method = env->GetMethodID(
        builder_class, name,
        "(I)Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;");
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };
  auto call_bool = [&](const char* name, jboolean value) -> bool {
    const jmethodID method = env->GetMethodID(
        builder_class, name,
        "(Z)Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;");
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };

  const jfieldID platform_asset =
      env->GetFieldID(platform_class, "assetFolderPath", "Ljava/lang/String;");
  if (!platform_asset) {
    clear_java_exception(env, "StartGameParams platform asset path");
    return nullptr;
  }
  jstring asset = env->NewStringUTF(content);
  env->SetObjectField(platform, platform_asset, asset);
  env->DeleteLocalRef(asset);
  const jfieldID dpi = env->GetFieldID(platform_class, "dpiScale", "F");
  const jfieldID keyboard =
      env->GetFieldID(platform_class, "isKeyboardDevice", "Z");
  const jfieldID mouse = env->GetFieldID(platform_class, "isMouseDevice", "Z");
  const jfieldID touch = env->GetFieldID(platform_class, "isTouchDevice", "Z");
  const jfieldID viewport_height =
      env->GetFieldID(platform_class, "viewportHeightMm", "I");
  const jfieldID viewport_width =
      env->GetFieldID(platform_class, "viewportWidthMm", "I");
  if (!dpi || !keyboard || !mouse || !touch || !viewport_height ||
      !viewport_width) {
    clear_java_exception(env, "StartGameParams platform capabilities");
    return nullptr;
  }
  /* Keep the same hybrid capability advertisement used by InitParams.  The
   * input bridge still sends real SDL mouse/keyboard events; isTouchDevice is
   * only the Android capability bit that makes Roblox expose Movement Mode. */
  env->SetFloatField(platform, dpi, 1.0f);
  env->SetBooleanField(platform, keyboard, JNI_TRUE);
  env->SetBooleanField(platform, mouse, JNI_TRUE);
  env->SetBooleanField(platform, touch, JNI_TRUE);
  env->SetIntField(platform, viewport_height, 190);
  env->SetIntField(platform, viewport_width, 340);
  if (env->ExceptionCheck()) {
    clear_java_exception(env, "StartGameParams platform fields");
    return nullptr;
  }

  const char* const empty = "";
  /* The live Android client consumes an instance/job id through its
   * accessCode getter on this native bridge.  Keep gameId separate: placing
   * the id there makes the UGC coordinator finalize before NetworkClient is
   * created, even though the Java model accepts the field. */
  const std::string access_code = request.game_instance_id.value_or("");
  const std::string game_id;
  const std::string reserved_server_access_code =
      request.reserved_server_access_code.value_or("");
  const std::string launch_data = request.launch_data.value_or("");
  const std::string call_id = request.call_id.value_or("");
  /* Match the observed vi.j0 WebView path: an authenticated place join uses
   * type 1 only when no explicit access code is present; an instance/private
   * launch remains on the type-2 path. */
  const jint join_request_type =
      nuah_roblox_user_id() > 0 && access_code.empty() ? 1 : 2;
  const jlong place_id = static_cast<jlong>(std::stoll(request.place_id));
  const bool fields_ok =
      call_object("setSurface", "(Landroid/view/Surface;)"
                               "Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;",
                  surface) &&
      call_object("setPlatformParams", "(Lcom/roblox/engine/jni/model/PlatformParams;)"
                                           "Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;",
                  platform) &&
      /* vi/i0.C() deliberately passes null here.  The native bridge treats a
       * fabricated DeviceParams as a different contract and leaves its game
       * session adapter uninitialised. */
      call_object("setDeviceParams", "(Lcom/roblox/engine/jni/model/DeviceParams;)"
                                         "Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;",
                  nullptr) &&
      call_long("setConversationId", 0) &&
      call_long("setReferredByPlayerId", 0) &&
      call_string("setAccessCode", access_code.c_str()) &&
      call_string("setCallId", call_id.c_str()) &&
      call_string("setEventId", empty) &&
      call_string("setGameId", empty) &&
      call_string("setGameJoinContext", empty) &&
      call_bool("setIsUnder13", JNI_FALSE) &&
      call_string("setIsoContext", empty) &&
      call_string("setJoinAttemptId", empty) &&
      call_string("setJoinAttemptOrigin", empty) &&
      call_int("setJoinRequestType", join_request_type) &&
      call_string("setLaunchData", launch_data.c_str()) &&
      call_string("setLinkCode", empty) &&
      call_long("setPlaceId", place_id) &&
      call_string("setReferralPage", "WebView") &&
      call_string("setReservedServerAccessCode",
                  reserved_server_access_code.c_str()) &&
      call_long("setUserId", nuah_roblox_user_id()) &&
      call_string(
          "setUsername",
          (::getenv("NUAH_ROBLOX_USERNAME") ? ::getenv("NUAH_ROBLOX_USERNAME")
                                            : empty)) &&
      /* rh.y0.D0() is false for the normal desktop session, so the APK does
       * not set a VR activity.  Keep this nullable unless explicitly testing
       * the VR path. */
      call_object("setVrContext", "(Landroid/app/Activity;)"
                                  "Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;",
                  (::getenv("NUAH_ENABLE_VR") ? activity : nullptr));
  if (!fields_ok) return nullptr;
  const jmethodID build = env->GetMethodID(
      builder_class, "build",
      "()Lcom/roblox/engine/jni/autovalue/StartGameParams;");
  if (!build) {
    clear_java_exception(env, "StartGameParams.build");
    return nullptr;
  }
  jobject result = env->CallObjectMethod(builder, build);
  if (!result || env->ExceptionCheck()) {
    clear_java_exception(env, "StartGameParams.build");
    return nullptr;
  }
  return result;
}

jobject make_real_platform_params(JNIEnv* env, const char* content_path) {
  if (!env) return nullptr;
  jclass klass =
      env->FindClass("com/roblox/engine/jni/model/PlatformParams");
  const jmethodID ctor = klass ? env->GetMethodID(klass, "<init>", "()V")
                               : nullptr;
  if (!klass || !ctor) {
    clear_java_exception(env, "PlatformParams constructor");
    return nullptr;
  }
  jobject params = env->NewObject(klass, ctor);
  if (!params) {
    clear_java_exception(env, "PlatformParams allocation");
    return nullptr;
  }
  auto set_string = [&](const char* name, const char* value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "Ljava/lang/String;");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    jstring text = env->NewStringUTF(value ? value : "");
    env->SetObjectField(params, field, text);
    env->DeleteLocalRef(text);
    return !env->ExceptionCheck();
  };
  auto set_bool = [&](const char* name, jboolean value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "Z");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    env->SetBooleanField(params, field, value);
    return !env->ExceptionCheck();
  };
  auto set_int = [&](const char* name, jint value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "I");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    env->SetIntField(params, field, value);
    return !env->ExceptionCheck();
  };
  auto set_float = [&](const char* name, jfloat value) -> bool {
    const jfieldID field = env->GetFieldID(klass, name, "F");
    if (!field) {
      clear_java_exception(env, name);
      return false;
    }
    env->SetFloatField(params, field, value);
    return !env->ExceptionCheck();
  };
  const bool ok =
      set_string("assetFolderPath", content_path) &&
      set_float("dpiScale", 1.0f) &&
      set_bool("isKeyboardDevice", JNI_TRUE) &&
      set_bool("isMouseDevice", JNI_TRUE) &&
      set_bool("isTouchDevice", JNI_TRUE) &&
      set_int("viewportHeightMm", 190) &&
      set_int("viewportWidthMm", 340);
  if (!ok || env->ExceptionCheck()) {
    clear_java_exception(env, "PlatformParams fields");
    return nullptr;
  }
  return params;
}

/* Mirror AppShellManager.startApp's small AutoValue object.  Roblox's Java
 * path calls this after nativeAppBridgeV2InitWithParams and before the first
 * game Surface callback; jumping directly to StartGame leaves
 * SingleSurfaceApp in the uninitialized state and produces a ghost window. */
jobject make_real_start_app_params(JNIEnv* env, jobject surface,
                                   jobject platform_params) {
  if (!env || !surface || !platform_params) return nullptr;
  jclass params_class =
      env->FindClass("com/roblox/engine/jni/autovalue/StartAppParams");
  jclass builder_class =
      env->FindClass("com/roblox/engine/jni/autovalue/StartAppParams$Builder");
  if (!params_class || !builder_class) {
    clear_java_exception(env, "StartAppParams classes");
    return nullptr;
  }
  const jmethodID builder_method = env->GetStaticMethodID(
      params_class, "builder",
      "()Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;");
  if (!builder_method) {
    clear_java_exception(env, "StartAppParams.builder");
    return nullptr;
  }
  jobject builder = env->CallStaticObjectMethod(params_class, builder_method);
  if (!builder || env->ExceptionCheck()) {
    clear_java_exception(env, "StartAppParams.builder call");
    return nullptr;
  }
  auto call_object = [&](const char* name, const char* signature,
                        jobject value) -> bool {
    const jmethodID method = env->GetMethodID(builder_class, name, signature);
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };
  auto call_string = [&](const char* name, const char* value) -> bool {
    const jstring text = env->NewStringUTF(value ? value : "");
    if (!text) return false;
    const bool ok = call_object(
        name, "(Ljava/lang/String;)"
              "Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;",
        text);
    env->DeleteLocalRef(text);
    return ok;
  };
  auto call_long = [&](const char* name, jlong value) -> bool {
    const jmethodID method = env->GetMethodID(
        builder_class, name,
        "(J)Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;");
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };
  auto call_int = [&](const char* name, jint value) -> bool {
    const jmethodID method = env->GetMethodID(
        builder_class, name,
        "(I)Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;");
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };
  auto call_bool = [&](const char* name, jboolean value) -> bool {
    const jmethodID method = env->GetMethodID(
        builder_class, name,
        "(Z)Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;");
    if (!method) {
      clear_java_exception(env, name);
      return false;
    }
    builder = env->CallObjectMethod(builder, method, value);
    if (!builder || env->ExceptionCheck()) {
      clear_java_exception(env, name);
      return false;
    }
    return true;
  };
  const bool fields_ok =
      call_object("setSurface", "(Landroid/view/Surface;)"
                               "Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;",
                  surface) &&
      call_object("setPlatformParams",
                  "(Lcom/roblox/engine/jni/model/PlatformParams;)"
                  "Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;",
                  platform_params) &&
      call_string("setAppStarterPlace", "") &&
      call_string("setAppStarterScript", "") &&
      call_long("setAppUserId", nuah_roblox_user_id()) &&
      call_bool("setIsUnder13", JNI_FALSE) &&
      call_string("setUsername", "") &&
      call_int("setMembershipType", 0) &&
      call_string("setSelectedTheme", "Dark");
  if (!fields_ok) return nullptr;
  const jmethodID build = env->GetMethodID(
      builder_class, "build",
      "()Lcom/roblox/engine/jni/autovalue/StartAppParams;");
  if (!build) {
    clear_java_exception(env, "StartAppParams.build");
    return nullptr;
  }
  jobject result = env->CallObjectMethod(builder, build);
  if (!result || env->ExceptionCheck()) {
    clear_java_exception(env, "StartAppParams.build call");
    return nullptr;
  }
  return result;
}

bool install_device_static_params(JNIEnv* env) {
  if (!env) return false;
  jclass bridge = env->FindClass(
      "com/roblox/engine/jni/NativeGLJavaInterface");
  jclass params_class = env->FindClass(
      "com/roblox/engine/jni/model/DeviceStaticParams");
  if (!bridge || !params_class) {
    clear_java_exception(env, "DeviceStaticParams classes");
    return false;
  }
  const jmethodID ctor = env->GetMethodID(params_class, "<init>", "()V");
  const jmethodID setter = env->GetStaticMethodID(
      bridge, "setDeviceStaticParams",
      "(Lcom/roblox/engine/jni/model/DeviceStaticParams;)V");
  if (!ctor || !setter) {
    clear_java_exception(env, "DeviceStaticParams methods");
    return false;
  }
  jobject params = env->NewObject(params_class, ctor);
  if (!params) {
    clear_java_exception(env, "DeviceStaticParams allocation");
    return false;
  }
  auto string_field = [&](const char* name, const char* value) {
    const jfieldID field = env->GetFieldID(params_class, name,
                                           "Ljava/lang/String;");
    if (!field) return false;
    const jstring text = env->NewStringUTF(value);
    env->SetObjectField(params, field, text);
    env->DeleteLocalRef(text);
    return !env->ExceptionCheck();
  };
  const jfieldID cpu = env->GetFieldID(params_class, "cpu64Bit", "Z");
  const bool fields_ok =
      string_field("appBuildVariant", "release") &&
      string_field("appVersion", "Roblox") &&
      cpu && string_field("deviceName", "Nuah Linux PC") &&
      string_field("deviceSku", "x86_64") &&
      string_field("manufacturer", "Nuah") &&
      string_field("osVersion", "36") &&
      string_field("socModel", "x86_64");
  if (!fields_ok || env->ExceptionCheck()) {
    clear_java_exception(env, "DeviceStaticParams fields");
    return false;
  }
  env->SetBooleanField(params, cpu, JNI_TRUE);
  env->CallStaticVoidMethod(bridge, setter, params);
  if (env->ExceptionCheck()) {
    clear_java_exception(env, "DeviceStaticParams setter");
    return false;
  }
  return true;
}

std::vector<std::filesystem::path> image_candidates(
    const NativeLaunchOptions& options) {
  std::vector<std::filesystem::path> result;
  result.reserve(options.split_apks.size() + 1);
  result.push_back(options.apk);
  for (const auto& split : options.split_apks) result.push_back(split);
  if (options.split_apks.empty()) {
    const auto sibling = options.apk.parent_path() / "split_config.x86_64.apk";
    if (std::filesystem::is_regular_file(sibling)) result.push_back(sibling);
  }
  return result;
}

std::filesystem::path find_image(const NativeLaunchOptions& options) {
  constexpr const char* kMember = "lib/x86_64/libroblox.so";
  for (const auto& apk : image_candidates(options)) {
    if (!std::filesystem::is_regular_file(apk)) continue;
    try {
      (void)read_stored_apk_member(apk, kMember);
      return apk;
    } catch (const std::exception&) {
      // The base APK often contains no native image; continue with the ABI
      // split instead of treating that normal layout as a fatal error.
    }
  }
  throw std::runtime_error(
      "selected APK set has no x86_64 lib/x86_64/libroblox.so");
}

std::filesystem::path runtime_directory() {
  std::array<char, 4096> path{};
  const auto size = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size <= 0 || static_cast<std::size_t>(size) >= path.size() - 1) {
    throw std::runtime_error("cannot locate Nuah runtime directory");
  }
  return std::filesystem::path(std::string(path.data(), size)).parent_path();
}

/* Roblox's AssetProvider opens one SQLite cache for the whole app-private
 * data directory.  A second native-run against that directory does not get a
 * harmless independent cache: it contends on rbx-storage.db and can block
 * the render thread for seconds while the database recovery path runs.  Hold
 * one advisory lock for the entire runtime lifetime so duplicate launches
 * fail immediately and cleanly. The descriptor is intentionally inherited by
 * the isolated bootstrap child and released automatically when the process
 * exits. */
class RuntimeDataLock {
 public:
  explicit RuntimeDataLock(const std::filesystem::path& data_directory) {
    std::error_code error;
    std::filesystem::create_directories(data_directory, error);
    if (error) {
      throw std::runtime_error("cannot create Nuah data directory for runtime lock: " +
                               error.message());
    }
    const auto lock_path = data_directory / "nuah-runtime.lock";
    fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) {
      throw std::runtime_error("cannot open Nuah runtime lock: " +
                               std::string(std::strerror(errno)));
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const int saved_errno = errno;
      ::close(fd_);
      fd_ = -1;
      if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
        throw std::runtime_error(
            "another Nuah runtime already owns this data directory; "
            "close it before launching a second room");
      }
      throw std::runtime_error("cannot acquire Nuah runtime lock: " +
                               std::string(std::strerror(saved_errno)));
    }
    const std::string owner = std::to_string(static_cast<long long>(::getpid())) + "\n";
    (void)::ftruncate(fd_, 0);
    (void)::write(fd_, owner.data(), owner.size());
  }

  RuntimeDataLock(const RuntimeDataLock&) = delete;
  RuntimeDataLock& operator=(const RuntimeDataLock&) = delete;

  ~RuntimeDataLock() {
    if (fd_ >= 0) {
      (void)::flock(fd_, LOCK_UN);
      (void)::close(fd_);
    }
  }

 private:
  int fd_ = -1;
};

/* A killed native-run can leave AssetProvider's WAL for the next launch to
 * replay synchronously on its first SQLite open.  That recovery is visible
 * as a several-refresh render stall even though the Vulkan bridge is idle.
 * The profile lock above proves that no other Nuah runtime is using this
 * cache, so checkpoint a non-empty WAL before ART/Roblox starts.  Keep this
 * tiny and optional at the ABI boundary: use the host SQLite DSO only for
 * this preflight and let Android/ATL own every SQLite handle after it.
 *
 * No hard link is added to Nuah.  Minimal installs without host SQLite simply
 * skip the optimization, while NUAH_CACHE_CHECKPOINT=0 provides an explicit
 * diagnostic opt-out. */
void checkpoint_asset_cache(const std::filesystem::path& runtime_data_directory,
                            const std::filesystem::path& apk) {
  const char* enabled = std::getenv("NUAH_CACHE_CHECKPOINT");
  if (enabled && std::strcmp(enabled, "0") == 0) return;

  const auto app_data = runtime_data_directory / (apk.filename().string() + "_");
  const auto database = app_data / "files/appData/rbx-storage.db";
  const auto wal = std::filesystem::path(database.string() + "-wal");
  std::error_code error;
  if (!std::filesystem::is_regular_file(database, error) || error ||
      !std::filesystem::is_regular_file(wal, error) || error ||
      std::filesystem::file_size(wal, error) == 0 || error) {
    return;
  }

  using Sqlite3 = void;
  using Open = int (*)(const char*, Sqlite3**, int, const char*);
  using Exec = int (*)(Sqlite3*, const char*, int (*)(void*, int, char**, char**),
                       void*, char**);
  using Close = int (*)(Sqlite3*);
  constexpr int kOpenReadWrite = 0x00000002;
  constexpr int kOpenFullMutex = 0x00010000;

  void* library = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!library) library = ::dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
  if (!library) return;
  const auto open = reinterpret_cast<Open>(::dlsym(library, "sqlite3_open_v2"));
  const auto exec = reinterpret_cast<Exec>(::dlsym(library, "sqlite3_exec"));
  const auto close = reinterpret_cast<Close>(::dlsym(library, "sqlite3_close"));
  if (!open || !exec || !close) {
    ::dlclose(library);
    return;
  }

  Sqlite3* connection = nullptr;
  const int open_result =
      open(database.c_str(), &connection,
           kOpenReadWrite | kOpenFullMutex, nullptr);
  if (open_result == 0 && connection) {
    /* sqlite3_exec follows the same path as Android's PRAGMA implementation
     * and also handles a stale -shm file left by a killed process. */
    const int result = exec(connection, "PRAGMA wal_checkpoint(TRUNCATE);",
                            nullptr, nullptr, nullptr);
    if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::cerr << "nuah cache: WAL checkpoint result=" << result
                << " path=" << database << '\n';
    }
  } else if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
             trace && *trace) {
    std::cerr << "nuah cache: WAL checkpoint skipped (sqlite open result="
              << open_result << ") path=" << database << '\n';
  }
  if (connection) (void)close(connection);
  ::dlclose(library);
}

std::filesystem::path extract_roblox_image(const std::filesystem::path& apk) {
  const auto member = read_stored_apk_member(apk, "lib/x86_64/libroblox.so");
  char path[] = "/tmp/nuah-roblox-XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd < 0) throw std::runtime_error("cannot create temporary Roblox image");
  std::size_t offset = 0;
  while (offset < member.bytes.size()) {
    const auto written = ::write(fd, member.bytes.data() + offset,
                                 member.bytes.size() - offset);
    if (written <= 0) {
      ::close(fd);
      ::unlink(path);
      throw std::runtime_error("cannot write temporary Roblox image");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fchmod(fd, 0500) != 0 || ::close(fd) != 0) {
    ::unlink(path);
    throw std::runtime_error("cannot finalize temporary Roblox image");
  }
  return path;
}

void trace_start_game_params(JNIEnv* env, jobject params) {
  const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
  if (!trace || !*trace || !env || !params) return;
  jclass klass = env->GetObjectClass(params);
  if (!klass) return;
  const jmethodID access = env->GetMethodID(
      klass, "accessCode", "()Ljava/lang/String;");
  const jmethodID user = env->GetMethodID(klass, "userId", "()J");
  const jmethodID type = env->GetMethodID(klass, "joinRequestType", "()I");
  const jmethodID place = env->GetMethodID(klass, "placeId", "()J");
  jstring access_value = access
                            ? static_cast<jstring>(env->CallObjectMethod(params, access))
                            : nullptr;
  const jlong user_value = user ? env->CallLongMethod(params, user) : 0;
  const jint type_value = type ? env->CallIntMethod(params, type) : -1;
  const jlong place_value = place ? env->CallLongMethod(params, place) : 0;
  std::size_t access_size = 0;
  if (access_value && !env->ExceptionCheck()) {
    const char* value = env->GetStringUTFChars(access_value, nullptr);
    if (value) {
      access_size = std::strlen(value);
      env->ReleaseStringUTFChars(access_value, value);
    }
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  std::fprintf(stderr,
               "nuah native: StartGameParams access_bytes=%zu user_id=%lld "
               "join_type=%d place_id=%lld\n",
               access_size, static_cast<long long>(user_value),
               static_cast<int>(type_value), static_cast<long long>(place_value));
  if (access_value) env->DeleteLocalRef(access_value);
  env->DeleteLocalRef(klass);
}

/* ART's bionic linker resolves libroblox's `libandroid.so` dependency through
 * the ATL companion soname.  A profile created by native-run contains the APK
 * libraries, but not that framework companion; in that state the first
 * bionic_dlopen fails on AAssetManager_fromJava and Android caches the failed
 * libroblox load.  Keep the explicit ATL provider and the app namespace in
 * sync by staging the selected companion into the profile on every launch.
 * It is only ~90 KiB, so an atomic refresh is cheaper and safer than allowing
 * a stale ABI to survive across ATL upgrades. */
void stage_atl_android_companion(const std::filesystem::path& app_directory) {
  std::vector<std::filesystem::path> candidates;
  if (const char* configured = std::getenv("NUAH_ATL_NATIVE_DIR");
      configured && *configured) {
    candidates.emplace_back(std::filesystem::path(configured) /
                            "libandroid.so.0");
  }
  if (const char* configured = std::getenv("NUAH_ATL_HOME");
      configured && *configured) {
    candidates.emplace_back(std::filesystem::path(configured) / "natives" /
                            "libandroid.so.0");
  }
  for (const auto& source : candidates) {
    std::error_code source_error;
    if (!std::filesystem::is_regular_file(source, source_error) ||
        source_error)
      continue;
    const auto target_directory = app_directory / "lib";
    std::error_code directory_error;
    std::filesystem::create_directories(target_directory, directory_error);
    if (directory_error) return;
    const auto target = target_directory / "libandroid.so.0";
    const auto temporary = target_directory /
                           ("libandroid.so.0.nuah-new-" +
                            std::to_string(static_cast<long long>(::getpid())));
    std::error_code copy_error;
    std::filesystem::copy_file(
        source, temporary, std::filesystem::copy_options::overwrite_existing,
        copy_error);
    if (copy_error) return;
    std::error_code rename_error;
    std::filesystem::rename(temporary, target, rename_error);
    if (rename_error) {
      std::error_code ignored;
      std::filesystem::copy_file(
          source, target, std::filesystem::copy_options::overwrite_existing,
          ignored);
      std::filesystem::remove(temporary, ignored);
    }
    std::error_code permission_error;
    std::filesystem::permissions(
        target,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace, permission_error);
    if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::cerr << "nuah ATL: staged companion " << source << " -> " << target
                << '\n';
    }
    return;
  }
}

int run_nuah_jni(const NativeLaunchOptions& options,
                 const std::filesystem::path& apk) {
  const bool fast_mvp = fast_mvp_enabled();
  const bool init_only = [] {
    const char* value = ::getenv("NUAH_INIT_ONLY");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  std::string asset_apks;
  for (const auto& candidate : image_candidates(options)) {
    if (!std::filesystem::is_regular_file(candidate)) continue;
    if (!asset_apks.empty()) asset_apks += ':';
    asset_apks += std::filesystem::absolute(candidate).string();
  }
  if (asset_apks.empty() || ::setenv("NUAH_APK_PATHS", asset_apks.c_str(), 1) != 0) {
    throw std::runtime_error("cannot configure Android asset APK paths");
  }
  // ATL's Android API provider expects the same per-APK app-private root that
  // its normal launcher creates (<data>/<apk-name>_/).  Set it before ART is
  // created so Environment/AssetManager native initialization sees a real
  // directory.  Passing the parent data directory here makes the provider
  // fall back to /tmp/nuah and its AssetManager then dereferences an empty
  // ApkAssets list.
  const auto app_data_directory =
      std::filesystem::absolute(options.data_directory.value_or(
          std::filesystem::temp_directory_path() / "nuah-data")) /
      (options.apk.filename().string() + "_");
  std::error_code app_data_error;
  std::filesystem::create_directories(app_data_directory, app_data_error);
  if (app_data_error ||
      ::setenv("ANDROID_APP_DATA_DIR", app_data_directory.c_str(), 1) != 0) {
    throw std::runtime_error("cannot configure Android app-private data directory");
  }
  stage_atl_android_companion(app_data_directory);
  configure_mesa_shader_cache(app_data_directory);
  configure_mesa_submit_thread();
  // Reuse ATL's APK-native extraction routine. This is the Android contract
  // System.loadLibrary expects; do not fabricate host substitutes for app
  // libraries such as libzstd-jni.
  (void)prepare_atl_native_libraries(options);
  const auto app_library_directory = app_data_directory / "lib";
  // The installed ATL linker owns the process's one libc/TLS domain. Keep
  // this path limited to extracted app libraries; accepting an inherited
  // private libc path would load a second Bionic provider and corrupt ART.
  std::string bionic_library_path;
  /* When the caller selects an installed ATL bundle, its matching
   * libandroid.so.0 must precede any framework DSO cached in an older app
   * profile.  Otherwise the provider's ApplyStyle relocation can bind to a
   * mismatched ABI.  App libraries are still included immediately after the
   * selected framework directory. */
  const char* atl_home = std::getenv("NUAH_ATL_HOME");
  const char* atl_native = std::getenv("NUAH_ATL_NATIVE_DIR");
  std::filesystem::path atl_native_directory;
  if (atl_native && *atl_native) {
    atl_native_directory = atl_native;
  } else if (atl_home && *atl_home) {
    atl_native_directory = std::filesystem::path(atl_home) / "natives";
  }
  if (!atl_native_directory.empty() &&
      std::filesystem::is_regular_file(
          atl_native_directory / "libandroid.so.0")) {
    bionic_library_path = atl_native_directory.string() + ":";
  }
  /* libroblox.so has direct NDK dependencies (libandroid.so, libvulkan.so,
   * and libmediandk.so).  They are host-side Nuah providers, not Android
   * linker images: their glibc relocations (including IFUNC/TLS forms) are
   * intentionally outside ATL's Android relocation parser.  libhybris has
   * already opened them and bionic_translation's try_glibc path can reuse
   * those handles by soname.  Do not put the host provider directory in
   * BIONIC_LD_LIBRARY_PATH; doing so makes the linker mmap the host DSO as an
   * Android image, cache its relocation failure, and poison System.loadLibrary.
   * The dependency placeholder directory remains owned by HYBRIS_LD_LIBRARY_PATH
   * for the libhybris side only. */
  /* The Nuah framework provider is a host DSO with two deliberately small
   * companion objects (libnuah_host_bridge.so and libnuah_android_registry.so).
   * ATL's bionic linker does not apply the CMake RUNPATH when it resolves a
   * DT_NEEDED entry, so expose the build root explicitly.  Without this the
   * provider file is opened but its export table is never linked into the
   * Android namespace, which looks like a missing AAssetManager symbol.
   */
  const auto nuah_runtime_directory = runtime_directory();
  if (std::filesystem::is_regular_file(
          nuah_runtime_directory / "libnuah_host_bridge.so"))
    bionic_library_path += nuah_runtime_directory.string() + ":";
  bionic_library_path +=
      app_library_directory.string() + ":" + app_data_directory.string() + "**";
  /* libtranslation_layer_main.so has one small host-side dependency that is
   * not an APK library: libandroidfw.so (ApplyStyle/Theme_applyStyle).  The
   * Android linker does not honor the ELF RUNPATH when resolving this
   * provider, so make the installed ART host directory explicit.  Keep the
   * app-private directories first; this does not reintroduce a second libc.
   */
  std::vector<std::filesystem::path> host_android_dirs;
  if (const char* configured = std::getenv("NUAH_ART_LIBRARY");
      configured && *configured) {
    host_android_dirs.emplace_back(std::filesystem::path(configured).parent_path());
  }
  if (const char* configured_dir = std::getenv("NUAH_ART_LIBRARY_DIR");
      configured_dir && *configured_dir) {
    host_android_dirs.emplace_back(configured_dir);
  }
  host_android_dirs.emplace_back("/usr/local/lib64/art");
  for (const auto& directory : host_android_dirs) {
    if (!std::filesystem::is_regular_file(directory / "libandroidfw.so"))
      continue;
    bionic_library_path += ":" + directory.string();
    break;
  }
  if (::setenv("BIONIC_LD_LIBRARY_PATH", bionic_library_path.c_str(), 1) != 0) {
    throw std::runtime_error("cannot configure Android app native-library path");
  }
  /* RTLD_NOW resolves libroblox's pthread relocations while it is opened. A
   * late NUAH_ANDROID_SYNC set (after JNI preparation) cannot change those
   * already-bound function pointers. Bind the pointer-tagged API-36 bridge
   * before relocation by default; set NUAH_ANDROID_SYNC_EARLY=0 only for the
   * old constructor-safe/host-libc comparison. */
  const char* early_sync = ::getenv("NUAH_ANDROID_SYNC_EARLY");
  const bool disable_early_sync =
      early_sync && *early_sync && std::strcmp(early_sync, "0") == 0;
  if (!disable_early_sync && !::getenv("NUAH_ANDROID_SYNC")) {
    if (::setenv("NUAH_ANDROID_SYNC", "1", 1) != 0) {
      throw std::runtime_error("cannot enable early Android synchronization");
    }
  }
  report_bootstrap_stage("ANDROID_DLOPEN_CONSTRUCTORS");
  auto image = load_apk_library(apk, "lib/x86_64/libroblox.so");
  // Do not invoke JNI_OnLoad here.  The same app image is opened by ATL's
  // System.loadLibrary from GameActivity; ART invokes JNI_OnLoad as part of
  // that call.  Calling it manually first initializes Roblox/WebRTC twice
  // (the second pass aborts on its global JavaVM guard).  The small settings
  // contract below is bound explicitly until ART performs the one real load.
  report_bootstrap_stage("PRE_JNI_BIND");
  std::unique_ptr<NuahNativeSession, decltype(&nuah_native_session_destroy)>
      session(nullptr, nuah_native_session_destroy);
  session.reset(nuah_native_session_create());
  if (!session) throw std::runtime_error("cannot create Nuah native session");
  // Keep the installed libdl_bio/libc_bio pair untouched.  Redirect only
  // Roblox's already-relocated property slot so the Java façade and native
  // feature gates observe the same API level without a second TLS domain.
  // The relocation is optional: some libhybris builds return a resolver
  // trampoline for bionic_dladdr until the first Java callback.  Never let
  // that diagnostic normalization crash the real bootstrap.
  if (!std::getenv("NUAH_DISABLE_PROPERTY_PATCH"))
    (void)patch_loaded_module_property_import(image);
  NuahJvm* jvm = nuah_native_session_jvm(session.get());
  using JniOnLoadFn = jint (*)(JavaVM*, void*);
  if (auto* jni_onload = reinterpret_cast<JniOnLoadFn>(image.symbol("JNI_OnLoad"))) {
    if (JavaVM* vm = reinterpret_cast<JavaVM*>(nuah_jvm_java_vm(jvm))) {
      jint version = jni_onload(vm, nullptr);
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
        std::fprintf(stderr, "nuah native: JNI_OnLoad initialized with version %d\n", version);
      }
    }
  }
  // MainGameActivity.onCreate runs Roblox's own AppManager before the
  // GameActivity native handoff. Its settings methods are exported by this
  // image but are not discoverable through ART's class-loader lookup because
  // libroblox was loaded through libhybris. Register the small pre-create
  // contract explicitly; the later graphics/input methods remain Roblox's
  // own JNI_OnLoad registrations.
  const auto bind_roblox_native = [&](const char* klass, const char* name,
                                      const char* signature,
                                      const char* symbol) {
    if (void* function = image.symbol(symbol)) {
      (void)nuah_jvm_bind_native(jvm, klass, name, signature, function);
    }
  };
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface", "nativeSetBaseUrl",
      "(Ljava/lang/String;Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetBaseUrl");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeSetRobloxChannel", "(Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetRobloxChannel");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeSetExceptionReasonFilename", "(Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_"
      "nativeSetExceptionReasonFilename");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeOverrideChannelPlatformName", "(Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_"
      "nativeOverrideChannelPlatformName");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeOverrideChannelPlatformName2", "(Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_"
      "nativeOverrideChannelPlatformName2");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface", "nativeInitFastLog",
      "()V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeInitFastLog");
  // MainGameActivity.onCreate initializes Crashpad through rh.w0.g0 before the
  // native engine starts.  These exports are present but ART cannot resolve
  // them through its class-loader search (libroblox was loaded via libhybris),
  // so register them as part of the pre-create contract.
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface", "nativeInitCrashpad",
      "(Lcom/roblox/engine/jni/model/NativeInitCrashpadParams;)Z",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeInitCrashpad");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeInitAppCrashpadReporter",
      "(Lcom/roblox/engine/jni/model/NativeInitCrashpadParams;)Z",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_"
      "nativeInitAppCrashpadReporter");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeRunCrashpadHandler", "([Ljava/lang/String;)I",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeRunCrashpadHandler");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeSetRobloxVersion", "(Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_"
      "nativeSetRobloxVersion");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface", "nativeSetUserId",
      "(Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetUserId");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeSetCacheDirectory", "(Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_"
      "nativeSetCacheDirectory");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeSettingsInterface",
      "nativeSetFilesDirectory", "(Ljava/lang/String;)V",
      "Java_com_roblox_engine_jni_NativeSettingsInterface_"
      "nativeSetFilesDirectory");
  // MainGameActivity and Roblox's application bootstrap install the same
  // callback before they create the native bridge.  Keep this binding in the
  // pre-bootstrap phase, matching the working Sober path; waiting until
  // after Application.onCreate leaves the Java-side callback table with a
  // null native entry during startup.
  bind_roblox_native(
      "com/roblox/engine/jni/NativeGLInterface", "nativePassKeyEvent",
      "(ZIIZ)V",
      "Java_com_roblox_engine_jni_NativeGLInterface_nativePassKeyEvent");
  // Sober's desktop-input adapter bypasses a synthetic touchscreen event for
  // mouse-capable devices and calls these exact Roblox entry points. Register
  // them before MainGameActivity is created so ART can resolve the static
  // methods when Nuah dispatches SDL events later on the same Java thread.
  bind_roblox_native(
      "com/roblox/engine/jni/NativeInputInterface", "nativePassMouseMove",
      "(FFFF)V",
      "Java_com_roblox_engine_jni_NativeInputInterface_nativePassMouseMove");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeInputInterface", "nativePassMouseButton",
      "(FFZI)V",
      "Java_com_roblox_engine_jni_NativeInputInterface_nativePassMouseButton");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeInputInterface", "nativePassMouseWheel",
      "(FFF)V",
      "Java_com_roblox_engine_jni_NativeInputInterface_nativePassMouseWheel");
  bind_roblox_native(
      "com/roblox/engine/jni/NativeInputInterface",
      "nativeGetMainWindowIsMouseLockedCenter", "()Z",
      "Java_com_roblox_engine_jni_NativeInputInterface_"
      "nativeGetMainWindowIsMouseLockedCenter");
  g_roblox_set_multiple_cookies =
      reinterpret_cast<RobloxSetMultipleCookies>(image.symbol(
          "Java_com_roblox_engine_jni_NativeSettingsInterface_"
          "nativeSetMultipleCookies"));
  g_roblox_get_cookies_for_domain =
      reinterpret_cast<RobloxGetCookiesForDomain>(image.symbol(
          "Java_com_roblox_engine_jni_NativeSettingsInterface_"
          "nativeGetCookiesForDomain"));
  if (!nuah_jvm_bind_native(
          jvm, "com/roblox/engine/jni/NativeSettingsInterface",
          "nativeGetCookiesInNetscapeFormat",
          "(Ljava/lang/String;)Ljava/lang/String;",
          reinterpret_cast<void*>(&nuah_native_get_cookies_netscape)) ||
      !nuah_jvm_bind_native(
          jvm, "com/roblox/engine/jni/NativeSettingsInterface",
          "nativeGetCookiesForDomain", "(Ljava/lang/String;)Ljava/lang/String;",
          reinterpret_cast<void*>(&nuah_native_get_cookies_for_domain)) ||
      !nuah_jvm_bind_native(
          jvm, "com/roblox/engine/jni/NativeSettingsInterface",
          "nativeSetMultipleCookies", "(Ljava/lang/String;Ljava/lang/String;)V",
          reinterpret_cast<void*>(&nuah_native_set_cookies))) {
    throw std::runtime_error("Roblox cookie boundary registration failed");
  }
  bind_roblox_native(
      "com/roblox/client/startup/MainGameActivity", "nativeSetAssetPath",
      "(Ljava/lang/String;)V",
      "Java_com_roblox_client_startup_MainGameActivity_nativeSetAssetPath");
  void* running_architecture = image.symbol(
      "Java_com_roblox_engine_jni_NativeSettingsInterface_"
      "getRunningArchitecture");
  if (!running_architecture)
    running_architecture = reinterpret_cast<void*>(
        &nuah_native_get_running_architecture);
  if (!nuah_jvm_bind_native(
          jvm, "com/roblox/engine/jni/NativeSettingsInterface",
          "getRunningArchitecture", "()I", running_architecture)) {
    throw std::runtime_error(
        "NativeSettingsInterface.getRunningArchitecture registration failed");
  }
  report_bootstrap_stage("APPLICATION_STATE_SETUP");
  if (!nuah_jvm_dispatch_application_create(jvm)) {
    throw std::runtime_error("Roblox application state setup failed");
  }
  auto* bootstrap_env =
      reinterpret_cast<JNIEnv*>(nuah_jvm_jni_env(jvm));
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr, "nuah native: DeviceStaticParams begin\n");
  if (!install_device_static_params(bootstrap_env)) {
    throw std::runtime_error(
        "Roblox DeviceStaticParams contract could not be initialized");
  }
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr, "nuah native: DeviceStaticParams done\n");
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr, "nuah native: cookie prime begin\n");
  prime_roblox_cookie_store(bootstrap_env, true);
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr, "nuah native: cookie prime done\n");

  // A JNI version alone only proves that the library accepted a table.  The
  // Sober capture shows that the usable native boundary is the smaller
  // GameActivity callback set registered by Roblox during JNI_OnLoad.  Keep
  // this gate on the same NuahJvm instance that was passed to JNI_OnLoad; a
  // second mock registry would make successful input delivery impossible.
  constexpr const char* kGameActivity =
      "com/google/androidgamesdk/GameActivity";
  constexpr const char* kInitializeSignature =
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
      "Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J";
  struct NativeRequirement {
    const char* member;
    const char* signature;
  };
  // The real ART adapter can resolve a Java method ID, but that does not
  // expose whether another JNI library already installed its native pointer.
  // Bind Roblox's exported entry point explicitly; RegisterNatives is the
  // documented way to attach it and avoids relying on ART's class-loader
  // library search (which cannot see our libhybris-loaded image).
  void* exported_initialize = image.symbol(
      "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
  if (!exported_initialize ||
      !nuah_jvm_bind_native(jvm, kGameActivity, "initializeNativeCode",
                            kInitializeSignature, exported_initialize)) {
    throw std::runtime_error(
        "libroblox.so exposes no bindable GameActivity initializeNativeCode");
  }
  std::unique_ptr<NuahWindowSession, decltype(&nuah_window_session_destroy)>
      window(nuah_window_session_create(options.width, options.height, "Roblox"),
             nuah_window_session_destroy);
  if (!window) throw std::runtime_error("cannot create Nuah SDL/Vulkan window");
  /* GameActivity_register expects the Surface class/object boundary to exist
   * before initializeNativeCode. Keep this true for init-only diagnostics as
   * well; SDL's offscreen driver makes the probe window side-effect free. */
  void* surface = nuah_native_session_surface(
      session.get(), nuah_window_session_native_window(window.get()));
  if (!surface) throw std::runtime_error("cannot create Nuah Android Surface façade");

  // Android would have delivered MainGameActivity.onCreate before any
  // GameActivity lifecycle callback. That method owns Roblox's AppManager
  // bootstrap (including NativeUserJavaInterface.setImplementation); calling
  // the native entry points directly without it leaves the app's Java-side
  // session object null and aborts in nativeAppBridgeSetInitParams.
  report_bootstrap_stage("ACTIVITY_ON_CREATE");
  /* Frida cannot safely spawn this ART/libhybris process, but a stopped
   * late-attach immediately before MainGameActivity.onCreate lets a
   * read-only probe observe Roblox's JNI flag registration. This is a
   * diagnostics-only opt-in and never changes the library image. */
  if (const char* pause = ::getenv("NUAH_PAUSE_BEFORE_ACTIVITY_CREATE");
      pause && *pause) {
    std::cerr << "nuah bootstrap: paused before MainGameActivity.onCreate (pid="
              << ::getpid() << ")\n";
    (void)::raise(SIGSTOP);
  }
  if (!nuah_jvm_dispatch_activity_create(jvm)) {
    throw std::runtime_error("MainGameActivity.onCreate failed");
  }
  /* MainGameActivity.onCreate creates GameActivity.H/I and its real
   * SurfaceHolder surface. Refresh the handoff after that point so the
   * subsequent start/update calls receive the same Java Surface object that
   * ATL delivered to GameActivity's native callbacks. */
  surface = nuah_native_session_surface(
      session.get(), nuah_window_session_native_window(window.get()));
  if (!surface)
    throw std::runtime_error("cannot obtain GameActivity holder Surface");

  const auto data_directory = options.data_directory.value_or(
      std::filesystem::temp_directory_path() / "nuah-data");
  std::error_code data_error;
  std::filesystem::create_directories(data_directory, data_error);
  if (data_error) {
    throw std::runtime_error("cannot create Nuah game data directory: " +
                             data_error.message());
  }
  report_bootstrap_stage("GAMEACTIVITY_INITIALIZE");
  if (nuah_jvm_capture_native_handle(jvm)) {
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
      std::cerr << "nuah native: reusing GameActivity.onCreate native handle\n";
    }
  } else if (!nuah_native_session_initialize_game(
                 session.get(), "com.roblox.client", data_directory.c_str())) {
    throw std::runtime_error(
        "GameActivity initializeNativeCode returned no native handle");
  }
  std::filesystem::path content_directory;
  std::filesystem::path extra_content_directory;
  if (const char* override_path = ::getenv("NUAH_CONTENT_PATH");
      override_path && *override_path) {
    content_directory = override_path;
    extra_content_directory = content_directory.parent_path() / "ExtraContent";
  } else {
    /* Prefer the APK-native extraction root.  Sober's shared directory is a
     * useful cache when populated, but on a fresh install its content and
     * ExtraContent directories are often empty placeholders. */
    const auto app_assets = app_data_directory / "files/assets";
    const auto app_assets_legacy = app_data_directory / "assets";
    const auto data_assets = data_directory / "assets";
    std::filesystem::path sober_assets;
    if (const char* home = ::getenv("HOME"); home && *home) {
      sober_assets = std::filesystem::path(home) /
                     ".var/app/org.vinegarhq.Sober/data/sober/assets";
    }
    const std::array<std::filesystem::path, 4> asset_roots = {
        app_assets, app_assets_legacy, data_assets, sober_assets};
    for (const auto& root : asset_roots) {
      const auto candidate = root / "content";
      const auto candidate_extra = root / "ExtraContent";
      if (!asset_tree_has_files(candidate)) continue;
      content_directory = candidate;
      extra_content_directory = candidate_extra;
      break;
    }
    if (content_directory.empty()) {
      content_directory = app_assets / "content";
      extra_content_directory = app_assets / "ExtraContent";
    }
  }
  std::filesystem::create_directories(content_directory, data_error);
  if (data_error) {
    throw std::runtime_error("cannot create Roblox content directory: " +
                             data_error.message());
  }
  auto content_path = std::filesystem::absolute(content_directory).string();
  if (!content_path.ends_with('/')) content_path += '/';
  /* InitParams and the Java façade read this environment value before the
   * later MainGameActivity.nativeSetAssetPath call.  Keep all three launch
   * boundaries on the same real directory instead of letting the first one
   * silently receive an empty path. */
  if (::setenv("NUAH_CONTENT_PATH", content_path.c_str(), 1) != 0) {
    throw std::runtime_error("cannot publish Roblox content path");
  }
  nuah_roblox_java_facade_set_content_path(content_path.c_str());
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::cerr << "nuah assets: content=" << content_path
              << " files=" << (asset_tree_has_files(content_directory) ? 1 : 0)
              << " extra=" << std::filesystem::absolute(extra_content_directory)
              << " extra_files="
              << (asset_tree_has_files(extra_content_directory) ? 1 : 0)
              << '\n';
  }
  prefetch_local_asset_pages(
      {content_directory, extra_content_directory});

  auto* env = reinterpret_cast<JNIEnv*>(nuah_jvm_jni_env(jvm));
  jclass main_activity =
      env->FindClass("com/roblox/client/startup/MainGameActivity");
  jclass native_gl =
      env->FindClass("com/roblox/engine/jni/NativeGLInterface");
  jobject init_params = make_real_init_params(
      env, reinterpret_cast<jobject>(nuah_jvm_game_activity(jvm)));
  using SetAssetPath = void (*)(JNIEnv*, jclass, jstring);
  using SetInitParams = void (*)(JNIEnv*, jclass, jobject);
  using InitClientSettings = jint (*)(JNIEnv*, jclass, jstring, jstring,
                                      jstring);
  using PostClientSettings = void (*)(JNIEnv*, jclass, jobject);
  using InitWithParams = void (*)(JNIEnv*, jclass, jobject);
  const auto set_asset_path = reinterpret_cast<SetAssetPath>(image.symbol(
      "Java_com_roblox_client_startup_MainGameActivity_nativeSetAssetPath"));
  const auto set_init_params = reinterpret_cast<SetInitParams>(image.symbol(
      "Java_com_roblox_client_startup_MainGameActivity_"
      "nativeAppBridgeSetInitParams"));
  const auto init_client_settings = reinterpret_cast<InitClientSettings>(
      image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_"
                   "nativeInitClientSettings"));
  const auto post_client_settings = reinterpret_cast<PostClientSettings>(
      image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_"
                   "nativePostClientSettingsLoadedInitialization3"));
  const auto init_with_params = reinterpret_cast<InitWithParams>(image.symbol(
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2InitWithParams"));
  if (!main_activity || !init_params || !set_asset_path ||
      (!native_gl || !init_with_params) ||
      (!fast_mvp && !set_init_params) || !init_client_settings ||
      !post_client_settings) {
    throw std::runtime_error(
        "Roblox MainGameActivity pre-start JNI contract is unavailable");
  }
  report_bootstrap_stage("ROBLOX_ASSET_PATH");
  set_asset_path(env, main_activity, env->NewStringUTF(content_path.c_str()));
  // Roblox installs its own native crash handlers during JNI_OnLoad. Re-arm
  // the supervisor before the first Roblox-specific lifecycle handoff so
  // faults in InitParams retain their native caller chain.
  if (!::getenv("NUAH_DISABLE_CRASH_HANDLER")) {
    nuah_bootstrap_diagnostics_install_signal_handler();
  }
  if (const char* pause = ::getenv("NUAH_PAUSE_BEFORE_INIT");
      pause && *pause) {
    std::cerr << "nuah bootstrap: paused before Roblox parameter handoff (pid="
              << ::getpid() << ")\n";
    (void)::raise(SIGSTOP);
  }
  /* Roblox's API-36 pthread_mutex_t is not glibc-compatible.  Keep the
   * loader/JNI bootstrap on the host synchronization objects, then enable
   * the tiny out-of-line Android mutex adapter only for the InitParams and
   * game runtime calls that pass Android mutex storage. */
  if (!::getenv("NUAH_ANDROID_SYNC") &&
      ::setenv("NUAH_ANDROID_SYNC", "1", 1) != 0) {
    throw std::runtime_error("cannot select Android synchronization adapter");
  }

  /* The Android activity fetches these flags before it initializes the data
   * model.  Calling V2 directly skips that contract and Roblox aborts with
   * "Can't initialize the TaskScheduler before flags have been loaded".
   * Accept a supplied response for real sessions. When no response was
   * supplied, reuse the APK's packaged applicationSettings response and apply
   * only Nuah's host-specific renderer/scheduler overrides. If the packaged
   * response is unavailable, fall back to the small Vulkan default. */
  report_bootstrap_stage("ROBLOX_CLIENT_SETTINGS_INIT");
  const char* settings_json = ::getenv("NUAH_CLIENT_SETTINGS_JSON");
  std::string settings_storage;
  std::string default_settings_storage;
  const char* disable_msaa_env = ::getenv("NUAH_DISABLE_MSAA");
  const bool disable_msaa =
      disable_msaa_env && *disable_msaa_env &&
      std::strcmp(disable_msaa_env, "0") != 0;
  if (const char* settings_path = ::getenv("NUAH_CLIENT_SETTINGS_PATH");
      settings_path && *settings_path) {
    std::ifstream settings_file(settings_path, std::ios::binary);
    if (!settings_file) {
      throw std::runtime_error("cannot open client-settings response: " +
                               std::string(settings_path));
    }
    std::ostringstream settings_stream;
    settings_stream << settings_file.rdbuf();
    settings_storage = settings_stream.str();
    if (settings_storage.empty()) {
      throw std::runtime_error("client-settings response is empty");
    }
    settings_json = settings_storage.c_str();
  }
  if (!settings_json || !*settings_json) {
    const char* requested_backend = ::getenv("NUAH_GRAPHICS_BACKEND");
    const bool prefer_opengl =
        requested_backend &&
        (std::strcmp(requested_backend, "opengl") == 0 ||
         std::strcmp(requested_backend, "gles") == 0 ||
         std::strcmp(requested_backend, "gl") == 0);
    /* Leave CPU headroom for ART, the Wayland event loop, and the Vulkan
     * driver.  On the two-core/four-thread host, the measured low-end profile
     * is three in-game workers, two AssetProvider workers, and a 4 ms texture
     * budget.  That finishes the initial scene fan-out faster without letting
     * the asset queue monopolise every logical CPU.
     * Larger hosts retain the less restrictive automatic profile.  Explicit
     * environment settings always win, so this remains easy to A/B test. */
    const unsigned int logical_cpus = std::thread::hardware_concurrency();
    const char* performance_mode = ::getenv("NUAH_PERFORMANCE_MODE");
    const bool quality_mode =
        performance_mode &&
        (std::strcmp(performance_mode, "quality") == 0 ||
         std::strcmp(performance_mode, "full") == 0);
    const bool requested_low_mode =
        performance_mode &&
        (std::strcmp(performance_mode, "low") == 0 ||
         std::strcmp(performance_mode, "balanced") == 0);
    /* Turbo is the native desktop default.  Keep the explicit balanced/low
     * and quality/full modes as opt-outs, rather than requiring every
     * launcher (or a WebKit URI handoff) to remember an environment export. */
    const bool turbo_mode =
        (!performance_mode || !*performance_mode ||
         std::strcmp(performance_mode, "turbo") == 0 ||
         std::strcmp(performance_mode, "fast") == 0);
    const bool low_end_profile =
        !quality_mode &&
        (turbo_mode || requested_low_mode ||
         (logical_cpus > 0 && logical_cpus <= 4));
    const char* governor = engine_governor_profile();
    const bool governor_balanced =
        governor && std::strcmp(governor, "balanced") == 0;
    unsigned long scheduler_threads =
        governor_balanced
            ? 2UL
            : low_end_profile
            ? (logical_cpus <= 1
                   ? 1UL
                   : std::min<unsigned long>(
                         3UL,
                         std::max<unsigned long>(
                             2UL, static_cast<unsigned long>(logical_cpus - 1))))
            : logical_cpus <= 1
                  ? 1UL
                  : std::min<unsigned long>(
                        4, std::max<unsigned int>(2, logical_cpus - 1));
    if (const char* raw = ::getenv("NUAH_TASK_THREADS"); raw && *raw) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(raw, &end, 10);
      if (end != raw && *end == '\0' && parsed >= 1 && parsed <= 64) {
        /* Two forced workers have repeatedly correlated with long render
         * hitches on the low-end Intel host.  Treat that legacy profile as
         * automatic instead of allowing an inherited shell variable to
         * re-enable it.  Other explicit A/B values remain available. */
        if (parsed == 2) {
          std::cerr << "nuah graphics: ignoring NUAH_TASK_THREADS=2; "
                       "using automatic worker selection\n";
        } else {
          scheduler_threads = parsed;
        }
      }
    }
    /* Match the host's 60-Hz compositor by default.  The target is still
     * capped by actual scene work and FIFO presentation; a higher target only
     * adds scheduler pressure on this host, but remains available through
     * NUAH_TARGET_FPS for high-refresh hosts and profiling. */
    /* Turbo is aimed at the measured 60-Hz Wayland/Intel host.  Requesting
     * 70 FPS there cannot increase visible output (FIFO remains 60 Hz), but
     * it can build a queue of GPU work and make vkAcquireNextImageKHR block
     * for an entire refresh or more.  Keep high-refresh hosts configurable
     * through NUAH_TARGET_FPS. */
    unsigned long target_fps = 60;
    if (const char* raw = ::getenv("NUAH_TARGET_FPS"); raw && *raw) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(raw, &end, 10);
      if (end != raw && *end == '\0' && parsed >= 30 && parsed <= 240)
        target_fps = parsed;
    }
    /* Texture decoding/transcoding belongs to Roblox, not Nuah.  On a small
     * host, however, the renderer can spend an entire refresh interval
     * draining the texture queue while AssetProvider callbacks compete for
     * the same cores.  Bound that work by default on the measured low-end
     * profile; explicit values still override it. */
    unsigned long render_texture_budget_ms =
        governor_balanced ? 2UL : (low_end_profile ? 4UL : 0UL);
    if (const char* raw = ::getenv("NUAH_RENDER_TEXTURE_BUDGET_MS");
        raw && *raw) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(raw, &end, 10);
      if (end != raw && *end == '\0' && parsed >= 1 && parsed <= 32)
        render_texture_budget_ms = parsed;
    }
    /* One AssetProvider worker is the measured sweet spot on the two-core
     * host.  Two workers compete with ART, the render job, and Mesa's submit
     * thread and turn the first populated scene into long frame gaps.  Keep
     * explicit NUAH_ASSET_PROVIDER_THREADS values authoritative. */
    unsigned long asset_provider_threads = low_end_profile ? 1UL : 0UL;
    if (const char* raw = ::getenv("NUAH_ASSET_PROVIDER_THREADS");
        raw && *raw) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(raw, &end, 10);
      if (end != raw && *end == '\0' && parsed >= 1 && parsed <= 4)
        asset_provider_threads = parsed;
    }
    /* The packaged value sleeps the AssetProvider workflow for 150 ms after
     * each pass. That is battery-friendly on Android but produces visible
     * desktop hitches while the first scene fans out. Remove that deliberate
     * sleep on the low-end desktop profile; the environment knob restores it
     * for compatibility testing. */
    unsigned long asset_workflow_sleep_us = low_end_profile ? 0UL : 150000UL;
    if (const char* raw = ::getenv("NUAH_ASSET_WORKFLOW_SLEEP_US");
        raw && *raw) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(raw, &end, 10);
      if (end != raw && *end == '\0' && parsed <= 1000000UL)
        asset_workflow_sleep_us = parsed;
    }
    /* The packaged Android settings currently set
     * DFFlagAlwaysSkipDiskCache=True. Keep that default unless the host
     * explicitly opts into persistent disk reads; the experiment is
     * reversible and does not alter texture formats or decoding. */
    const char* disk_cache = ::getenv("NUAH_ASSET_DISK_CACHE");
    const bool enable_disk_asset_cache =
        disk_cache && *disk_cache && std::strcmp(disk_cache, "0") != 0;
    /* The packaged Android response currently carries an experiment value
     * FFlagSlowDownRendering=True.  That is useful for a battery-oriented
     * mobile rollout, but it is a poor default for an interactive desktop
     * surface: it can deliberately insert render pacing even after the
     * scene is loaded.  Keep the shipped value authoritative by default and
     * expose one explicit A/B switch until the effect is measured on each
     * driver. */
    const char* fast_render = ::getenv("NUAH_FAST_RENDER");
    const bool fast_render_explicit = fast_render && *fast_render;
    const bool disable_slow_rendering =
        fast_render_explicit
            ? std::strcmp(fast_render, "0") != 0
            : turbo_mode;
    /* Roblox's Android settings disable the priority-aware AssetProvider
     * worker factory.  That is reasonable on a phone's unified scheduler,
     * but it lets background asset callbacks compete equally with the render
     * job on a small desktop CPU.  Keep the shipped setting by default and
     * make the desktop-priority experiment explicit until it is validated
     * across rooms. */
    const char* asset_background = ::getenv("NUAH_ASSET_BACKGROUND");
    const bool asset_background_explicit = asset_background && *asset_background;
    const bool prioritize_render_over_assets =
        asset_background_explicit
            ? std::strcmp(asset_background, "0") != 0
            : turbo_mode;
    /* The Android client currently enables a blocking RuntimeContent
     * transcode call.  On a desktop render loop that can turn a cold KTX2
     * result into a missed refresh even though the transcoder itself is
     * available on Roblox's worker queue.  Keep the shipped behavior as the
     * baseline and expose an explicit, reversible A/B switch; this does not
     * install a second Basis/KTX2 implementation. */
    const char* async_transcode =
        ::getenv("NUAH_ASSET_TRANSCODE_ASYNC");
    const bool async_transcode_explicit = async_transcode && *async_transcode;
    const bool allow_async_transcode =
        async_transcode_explicit
            ? std::strcmp(async_transcode, "0") != 0
            : low_end_profile;
    /* TexturePackGenerator performs synchronous Vulkan pipeline creation from
     * Roblox's render/FunctionMarshal path.  Keep the shipped generator as
     * the default, but provide a narrowly scoped A/B switch so a host can
     * compare the ordinary texture path without patching libroblox.so. */
    const char* disable_texture_pack_generator_env =
        ::getenv("NUAH_DISABLE_TEXTURE_PACK_GENERATOR");
    const bool disable_texture_pack_generator =
        disable_texture_pack_generator_env &&
        *disable_texture_pack_generator_env &&
        std::strcmp(disable_texture_pack_generator_env, "0") != 0;
    /* Keep anti-aliasing as a reversible engine-settings A/B switch.  Do not
     * force Vulkan sample counts in the shim: Roblox must create matching
     * multisample attachments and resolve them itself. */
    /* Roblox's FRM quality override is the engine-owned way to reduce
     * shadows/LOD/material work without inventing a second renderer.  The
     * measured turbo profile uses the lowest quality level to keep the
     * populated-room render job from monopolising the two physical cores;
     * NUAH_FRM_QUALITY=0 restores the packaged quality for comparison. */
    unsigned long frm_quality = turbo_mode ? 1UL : 0UL;
    bool frm_quality_explicit = false;
    if (const char* raw = ::getenv("NUAH_FRM_QUALITY"); raw && *raw) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(raw, &end, 10);
      if (end != raw && *end == '\0' && parsed <= 21) {
        frm_quality = parsed;
        frm_quality_explicit = true;
      }
    }
    /* A 429 from a non-critical profile endpoint currently enters Roblox's
     * generic three-second retry queue. The request is background work, but
     * its completion can still hold a shared engine job and stop presents.
     * The desktop bridge therefore fails that background request fast by
     * default; NUAH_HTTP_BACKGROUND_NO_RETRY=0 restores Android retry policy
     * for compatibility experiments. Join POSTs retain their explicit
     * gamejoin retry rule. */
    const char* no_background_http_retry_env =
        ::getenv("NUAH_HTTP_BACKGROUND_NO_RETRY");
    const bool no_background_http_retry =
        !no_background_http_retry_env || !*no_background_http_retry_env ||
        std::strcmp(no_background_http_retry_env, "0") != 0;
    /* Profile lookups are optional for joining a place, but Roblox can wait
     * on them while the first scene is assembled. Keep the shipped timeout
     * unless a desktop experiment explicitly requests a bounded fast-fail
     * value. */
    unsigned long http_request_timeout_ms = 0;
    if (const char* raw = ::getenv("NUAH_HTTP_REQUEST_TIMEOUT_MS");
        raw && *raw) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(raw, &end, 10);
      if (end != raw && *end == '\0' && parsed >= 100 && parsed <= 60000)
        http_request_timeout_ms = parsed;
    }
    const char* disable_profile_configuration_env =
        ::getenv("NUAH_DISABLE_PROFILE_CONFIGURATION");
    const bool disable_profile_configuration_explicit =
        disable_profile_configuration_env &&
        *disable_profile_configuration_env;
    const bool disable_profile_configuration =
        disable_profile_configuration_explicit
            ? std::strcmp(disable_profile_configuration_env, "0") != 0
            : turbo_mode;
    static constexpr const char* kDesktopHttpRetryOverrides =
        "503:POST:gamejoin.roblox.com:/v1/{join-endpoint}:linear:10:6|"
        "429:POST:gamejoin.roblox.com:/v1/{join-endpoint}:linear:10:6|"
        "429:GET:users.roblox.com:/v1/users:none:0:0";
    std::ostringstream generated_settings;
    generated_settings
        << "{\"applicationSettings\":{"
        << "\"FFlagDebugGraphicsPreferOpenGL\":"
        << (prefer_opengl ? "true" : "false") << ','
        << "\"FFlagDebugGraphicsPreferVulkan\":"
        << (prefer_opengl ? "false" : "true") << ','
        << "\"FFlagDebugGraphicsDisableVulkan\":"
        << (prefer_opengl ? "true" : "false") << ','
       << "\"FIntTaskSchedulerAutoThreadLimit\":\""
       << scheduler_threads << "\","
       << "\"DFIntTaskSchedulerJobInGameThreads\":\""
       << scheduler_threads << "\","
       << "\"FIntTaskSchedulerAsyncTasksMinimumThreadCount\":\"1\","
        << "\"FIntTaskSchedulerThreadMin\":\"0\","
        << "\"DFIntTaskSchedulerTargetFps\":\""
        << target_fps << "\"}}";
    default_settings_storage = generated_settings.str();
    settings_storage = packaged_client_settings(app_data_directory);
    if (!settings_storage.empty()) {
      const std::string graphics_opengl = prefer_opengl ? "true" : "false";
      const std::string graphics_vulkan = prefer_opengl ? "false" : "true";
      const std::string graphics_disabled = prefer_opengl ? "true" : "false";
      set_client_setting(settings_storage, "FFlagDebugGraphicsPreferOpenGL",
                         graphics_opengl);
      set_client_setting(settings_storage, "FFlagDebugGraphicsPreferVulkan",
                         graphics_vulkan);
      set_client_setting(settings_storage, "FFlagDebugGraphicsDisableVulkan",
                         graphics_disabled);
      set_client_setting(settings_storage, "FIntTaskSchedulerAutoThreadLimit",
                         "\"" + std::to_string(scheduler_threads) + "\"");
      set_client_setting(
          settings_storage, "DFIntTaskSchedulerJobInGameThreads",
          "\"" + std::to_string(scheduler_threads) + "\"");
      set_client_setting(
          settings_storage, "FIntTaskSchedulerAsyncTasksMinimumThreadCount",
          "\"1\"");
      set_client_setting(settings_storage, "FIntTaskSchedulerThreadMin",
                         "\"0\"");
      set_client_setting(settings_storage, "DFIntTaskSchedulerTargetFps",
                         "\"" + std::to_string(target_fps) + "\"");
      if (render_texture_budget_ms != 0) {
        set_client_setting(
            settings_storage, "FIntRenderTextureProcessingBudgetMilliseconds",
            "\"" + std::to_string(render_texture_budget_ms) + "\"");
      }
      if (asset_provider_threads != 0) {
        const std::string worker_count =
            "\"" + std::to_string(asset_provider_threads) + "\"";
        set_client_setting(
            settings_storage, "DFIntAssetProviderAssetCacheReadThreadCount",
            worker_count);
        set_client_setting(
            settings_storage, "DFIntAssetProviderCallbackExecutorThreadCount",
            worker_count);
      }
      set_client_setting(
          settings_storage,
          "DFIntAssetProviderWorkflowExecutorSleepMicroSeconds",
          "\"" + std::to_string(asset_workflow_sleep_us) + "\"");
      if (disk_cache && *disk_cache) {
        set_client_setting(settings_storage, "DFFlagAlwaysSkipDiskCache",
                           enable_disk_asset_cache ? "false" : "true");
      }
      if (disable_slow_rendering)
        set_client_setting(settings_storage, "FFlagSlowDownRendering", "false");
      if (prioritize_render_over_assets) {
        set_client_setting(
            settings_storage,
            "FFlagAssetProviderDisablePrioAwareStdWorkerThreadTaskFactory",
            "false");
        set_client_setting(
            settings_storage,
            "FFlagAssetProviderDisablePriorityAwareDeferredWritesTaskFactory",
            "false");
      }
      if (allow_async_transcode)
        set_client_setting(settings_storage,
                           "FFlagSimRuntimeContentTranscodeBlockingCall",
                           "false");
      if (disable_texture_pack_generator) {
        set_client_setting(settings_storage,
                           "FFlagEnableTexturePackGeneratorOnClient2",
                           "false");
        set_client_setting(settings_storage, "FFlagTexturePackGeneratorUseRaw",
                           "false");
        set_client_setting(settings_storage,
                           "DFFlagDisableRccTexturePackGenerator", "true");
      }
      if (disable_msaa) {
        set_client_setting(settings_storage, "DebugForceMSAASamples", "\"0\"");
        set_client_setting(settings_storage,
                           "DebugFRMOptionalMSAALevelOverride", "\"0\"");
      }
      if (frm_quality != 0)
        set_client_setting(
            settings_storage, "DFIntDebugFRMQualityLevelOverride",
            "\"" + std::to_string(frm_quality) + "\"");
      if (no_background_http_retry)
        set_client_setting(
            settings_storage, "DFStringHttpRetryOverridesDsv2",
            std::string("\"") + kDesktopHttpRetryOverrides + "\"");
      if (no_background_http_retry)
        set_client_setting(settings_storage,
                           "FStringHttpRetryOverridesDsv2Static",
                           std::string("\"") + kDesktopHttpRetryOverrides +
                               "\"");
      if (no_background_http_retry)
        set_client_setting(
            settings_storage, "DFStringHttpRetryOverridesDsv2_PlaceFilter",
            std::string("\"") + kDesktopHttpRetryOverrides + "\"");
      if (no_background_http_retry)
        set_client_setting(
            settings_storage,
            "FStringHttpRetryOverridesDsv2Static_PlaceFilter",
            std::string("\"") + kDesktopHttpRetryOverrides + "\"");
      if (no_background_http_retry)
        set_client_setting(settings_storage, "DFIntHttpRbxApiMaxRetryCount",
                           "\"0\"");
      if (http_request_timeout_ms != 0) {
        const std::string timeout =
            "\"" + std::to_string(http_request_timeout_ms) + "\"";
        set_client_setting(settings_storage,
                           "DFIntHttpServiceRequestTimeoutMs", timeout);
        set_client_setting(settings_storage,
                           "DFIntHttpResponseDefaultTimeoutMillis", timeout);
      }
      if (disable_profile_configuration) {
        set_client_setting(settings_storage,
                           "FFlagPlayersGetProfileConfigurationEnabled",
                           "false");
        set_client_setting(
            settings_storage,
            "FFlagPlayersGetProfileConfigurationFromUserIdEnabled", "false");
        set_client_setting(settings_storage,
                           "DFFlagPopulateUserInformationForPlayers", "false");
      }
      settings_json = settings_storage.c_str();
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::cerr << "nuah graphics: using packaged applicationSettings "
                     "(host overrides applied) backend="
                  << (prefer_opengl ? "opengl" : "vulkan") << '\n';
        if (render_texture_budget_ms != 0 || asset_provider_threads != 0) {
          std::cerr << "nuah graphics: scheduler_threads="
                    << scheduler_threads << " texture_budget_ms="
                    << render_texture_budget_ms
                    << " asset_provider_threads=" << asset_provider_threads
                    << '\n';
        }
        if (disk_cache && *disk_cache)
          std::cerr << "nuah graphics: disk_asset_cache="
                    << (enable_disk_asset_cache ? "enabled" : "disabled")
                    << '\n';
        if (disable_slow_rendering)
          std::cerr << "nuah graphics: slow_rendering=disabled ("
                    << (fast_render_explicit ? "A/B" : "turbo default")
                    << ")\n";
        if (prioritize_render_over_assets)
          std::cerr << "nuah graphics: asset_workers=priority-aware ("
                    << (asset_background_explicit ? "A/B" : "turbo default")
                    << ")\n";
        std::cerr << "nuah graphics: asset_workflow_sleep_us="
                  << asset_workflow_sleep_us << '\n';
        if (allow_async_transcode)
          std::cerr << "nuah graphics: blocking texture transcode=disabled ("
                    << (async_transcode_explicit ? "A/B" : "low-end default")
                    << ")\n";
        if (disable_texture_pack_generator)
          std::cerr << "nuah graphics: TexturePackGenerator=disabled (A/B)\n";
        if (frm_quality != 0)
          std::cerr << "nuah graphics: FRM quality=" << frm_quality << ' '
                    << (frm_quality_explicit ? "(A/B)" : "(turbo default)")
                    << "\n";
        if (no_background_http_retry)
          std::cerr << "nuah network: profile HTTP 429 retries disabled ("
                    << (no_background_http_retry_env && *no_background_http_retry_env
                            ? "A/B"
                            : "desktop default")
                    << ")\n";
        if (http_request_timeout_ms != 0)
          std::cerr << "nuah network: HTTP request timeout="
                    << http_request_timeout_ms << " ms (A/B)\n";
        if (disable_profile_configuration)
          std::cerr << "nuah network: player profile configuration disabled ("
                    << (disable_profile_configuration_explicit ? "A/B"
                                                                : "turbo default")
                    << ")\n";
      }
    } else {
      if (render_texture_budget_ms != 0) {
        set_client_setting(
            default_settings_storage,
            "FIntRenderTextureProcessingBudgetMilliseconds",
            "\"" + std::to_string(render_texture_budget_ms) + "\"");
      }
      if (asset_provider_threads != 0) {
        const std::string worker_count =
            "\"" + std::to_string(asset_provider_threads) + "\"";
        set_client_setting(default_settings_storage,
                           "DFIntAssetProviderAssetCacheReadThreadCount",
                           worker_count);
        set_client_setting(
            default_settings_storage,
            "DFIntAssetProviderCallbackExecutorThreadCount", worker_count);
      }
      set_client_setting(
          default_settings_storage,
          "DFIntAssetProviderWorkflowExecutorSleepMicroSeconds",
          "\"" + std::to_string(asset_workflow_sleep_us) + "\"");
      if (disk_cache && *disk_cache) {
        set_client_setting(default_settings_storage,
                           "DFFlagAlwaysSkipDiskCache",
                           enable_disk_asset_cache ? "false" : "true");
      }
      if (disable_slow_rendering)
        set_client_setting(default_settings_storage, "FFlagSlowDownRendering",
                           "false");
      if (prioritize_render_over_assets) {
        set_client_setting(
            default_settings_storage,
            "FFlagAssetProviderDisablePrioAwareStdWorkerThreadTaskFactory",
            "false");
        set_client_setting(
            default_settings_storage,
            "FFlagAssetProviderDisablePriorityAwareDeferredWritesTaskFactory",
            "false");
      }
      if (allow_async_transcode)
        set_client_setting(
            default_settings_storage,
            "FFlagSimRuntimeContentTranscodeBlockingCall", "false");
      if (disable_texture_pack_generator) {
        set_client_setting(default_settings_storage,
                           "FFlagEnableTexturePackGeneratorOnClient2",
                           "false");
        set_client_setting(default_settings_storage,
                           "FFlagTexturePackGeneratorUseRaw", "false");
        set_client_setting(default_settings_storage,
                           "DFFlagDisableRccTexturePackGenerator", "true");
      }
      if (disable_msaa) {
        set_client_setting(default_settings_storage, "DebugForceMSAASamples",
                           "\"0\"");
        set_client_setting(default_settings_storage,
                           "DebugFRMOptionalMSAALevelOverride", "\"0\"");
      }
      if (frm_quality != 0)
        set_client_setting(
            default_settings_storage, "DFIntDebugFRMQualityLevelOverride",
            "\"" + std::to_string(frm_quality) + "\"");
      if (no_background_http_retry)
        set_client_setting(
            default_settings_storage, "DFStringHttpRetryOverridesDsv2",
            std::string("\"") + kDesktopHttpRetryOverrides + "\"");
      if (no_background_http_retry)
        set_client_setting(default_settings_storage,
                           "FStringHttpRetryOverridesDsv2Static",
                           std::string("\"") + kDesktopHttpRetryOverrides +
                               "\"");
      if (no_background_http_retry)
        set_client_setting(
            default_settings_storage,
            "DFStringHttpRetryOverridesDsv2_PlaceFilter",
            std::string("\"") + kDesktopHttpRetryOverrides + "\"");
      if (no_background_http_retry)
        set_client_setting(
            default_settings_storage,
            "FStringHttpRetryOverridesDsv2Static_PlaceFilter",
            std::string("\"") + kDesktopHttpRetryOverrides + "\"");
      if (no_background_http_retry)
        set_client_setting(default_settings_storage,
                           "DFIntHttpRbxApiMaxRetryCount", "\"0\"");
      if (http_request_timeout_ms != 0) {
        const std::string timeout =
            "\"" + std::to_string(http_request_timeout_ms) + "\"";
        set_client_setting(default_settings_storage,
                           "DFIntHttpServiceRequestTimeoutMs", timeout);
        set_client_setting(default_settings_storage,
                           "DFIntHttpResponseDefaultTimeoutMillis", timeout);
      }
      if (disable_profile_configuration) {
        set_client_setting(default_settings_storage,
                           "FFlagPlayersGetProfileConfigurationEnabled",
                           "false");
        set_client_setting(
            default_settings_storage,
            "FFlagPlayersGetProfileConfigurationFromUserIdEnabled", "false");
        set_client_setting(default_settings_storage,
                           "DFFlagPopulateUserInformationForPlayers", "false");
      }
      settings_json = default_settings_storage.c_str();
        if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
            trace && *trace) {
        std::cerr << "nuah graphics: default backend="
                  << (prefer_opengl ? "opengl" : "vulkan") << '\n';
        std::cerr << "nuah graphics: scheduler_threads="
                  << scheduler_threads << " texture_budget_ms="
                  << render_texture_budget_ms
                  << " asset_provider_threads=" << asset_provider_threads
                  << '\n';
        if (disable_slow_rendering)
          std::cerr << "nuah graphics: slow_rendering=disabled ("
                    << (fast_render_explicit ? "A/B" : "turbo default")
                    << ")\n";
        if (prioritize_render_over_assets)
          std::cerr << "nuah graphics: asset_workers=priority-aware ("
                    << (asset_background_explicit ? "A/B" : "turbo default")
                    << ")\n";
        std::cerr << "nuah graphics: asset_workflow_sleep_us="
                  << asset_workflow_sleep_us << '\n';
        if (allow_async_transcode)
          std::cerr << "nuah graphics: blocking texture transcode=disabled ("
                    << (async_transcode_explicit ? "A/B" : "low-end default")
                    << ")\n";
        if (disable_texture_pack_generator)
          std::cerr << "nuah graphics: TexturePackGenerator=disabled (A/B)\n";
        if (frm_quality != 0)
          std::cerr << "nuah graphics: FRM quality=" << frm_quality << ' '
                    << (frm_quality_explicit ? "(A/B)" : "(turbo default)")
                    << "\n";
        if (no_background_http_retry)
          std::cerr << "nuah network: profile HTTP 429 retries disabled ("
                    << (no_background_http_retry_env && *no_background_http_retry_env
                            ? "A/B"
                            : "desktop default")
                    << ")\n";
        if (http_request_timeout_ms != 0)
          std::cerr << "nuah network: HTTP request timeout="
                    << http_request_timeout_ms << " ms (A/B)\n";
        if (disable_profile_configuration)
          std::cerr << "nuah network: player profile configuration disabled ("
                    << (disable_profile_configuration_explicit ? "A/B"
                                                                : "turbo default")
                    << ")\n";
      }
    }
  }
  if (disable_msaa) {
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::cerr << "nuah graphics: MSAA disabled via client settings\n";
    }
  }
  const jstring settings = env->NewStringUTF(settings_json);
  const jstring settings_signature = env->NewStringUTF("");
  const jstring settings_application = env->NewStringUTF("GoogleAndroidApp");
  const jint settings_status = init_client_settings(
      env, native_gl, settings, settings_signature, settings_application);
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::cerr << "nuah native: nativeInitClientSettings status="
              << settings_status << '\n';
  }
  if (settings_status != 0) {
    throw std::runtime_error("Roblox client-settings initialization failed with status " +
                             std::to_string(settings_status));
  }
  report_bootstrap_stage("ROBLOX_CLIENT_SETTINGS_POST_INIT");
  const jclass history_class = env->FindClass("java/util/ArrayList");
  const jobject history = history_class ? env->AllocObject(history_class) : nullptr;
  if (!history) throw std::runtime_error("cannot create empty client-settings history list");
  /* The post-init callback is not part of the minimal Sober launch contract
   * and, on this Roblox build, can race the first game bootstrap.  Keep it
   * available for ABI experiments, but make the proven launch path the
   * default.  Set NUAH_CLIENT_SETTINGS_POST=1 to opt in. */
  const bool post_client_settings_enabled = [] {
    if (const char* enable = ::getenv("NUAH_CLIENT_SETTINGS_POST");
        enable && *enable && std::strcmp(enable, "0") != 0) {
      return true;
    }
    if (const char* skip = ::getenv("NUAH_SKIP_CLIENT_SETTINGS_POST");
        skip && *skip && std::strcmp(skip, "0") != 0) {
      return false;
    }
    return false;
  }();
  if (post_client_settings_enabled) {
    post_client_settings(env, native_gl, history);
  } else if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
             trace && *trace) {
    std::cerr << "nuah native: skipping client-settings post-init (set "
                 "NUAH_CLIENT_SETTINGS_POST=1 to opt in)\n";
  }

  /* The APK's AppShell setup calls these before it initializes the data model
   * (vi/e.E -> nativeGameGlobalInit -> nativeUpdateAdapterInit -> vi/e.j).
   * Keep the same order here; starting a game first leaves SingleSurfaceApp
   * without its global adapter and its later surface update dereferences that
   * missing state. */
  using GlobalInit = void (*)(JNIEnv*, jclass);
  using UpdateAdapterInit = void (*)(JNIEnv*, jclass);
  const auto global_init = reinterpret_cast<GlobalInit>(image.symbol(
      "Java_com_roblox_engine_jni_NativeGLInterface_nativeGameGlobalInit"));
  const auto update_adapter_init = reinterpret_cast<UpdateAdapterInit>(
      image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_"
                   "nativeUpdateAdapterInit"));
  report_bootstrap_stage("ROBLOX_GAME_GLOBAL_INIT");
  if (global_init) global_init(env, native_gl);
  if (update_adapter_init) update_adapter_init(env, native_gl);

  if (!fast_mvp) {
    report_bootstrap_stage("ROBLOX_INIT_PARAMS_DIAGNOSTIC");
    const char* skip_init = ::getenv("NUAH_SKIP_ROBLOX_INIT_PARAMS");
    if (!skip_init || !*skip_init || std::strcmp(skip_init, "0") == 0) {
      set_init_params(env, main_activity, init_params);
    } else if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
               trace && *trace) {
      std::cerr << "nuah native: skipping MainGameActivity parameter setter "
                   "for ABI comparison\n";
    }
  }

  /* MainGameActivity's AppManager performs this immediately after the init
   * setter.  Keep both halves of that contract even in the diagnostic path;
   * the setter alone leaves SingleSurfaceApp's render-session provider null,
   * which only becomes visible when updateSurface is called. */
  report_bootstrap_stage(fast_mvp ? "ROBLOX_FAST_V2_INIT"
                                  : "ROBLOX_GAME_V2_INIT");
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace) {
    std::cerr << "nuah native: NativeGLInterface V2 init\n";
  }
  init_with_params(env, native_gl, init_params);

  report_bootstrap_stage("ROBLOX_V2_INIT_COMPLETE");
  if (init_only) {
    std::cerr << "nuah native: V2 initialization completed (init-only)\n";
    return 0;
  }

  /* These are the only callbacks required to reach a first frame and basic
   * desktop input. Pause/stop/text-input remain optional until the MVP is
   * rendering. This mirrors GameActivity's documented callback boundary
   * without making unrelated Android APIs a launch prerequisite. */
  constexpr NativeRequirement kMvpRequirements[] = {
      {"onStartNative", "(J)V"},
      {"onResumeNative", "(J)V"},
      {"onSurfaceCreatedNative", "(JLandroid/view/Surface;)V"},
      {"onSurfaceChangedNative", "(JLandroid/view/Surface;III)V"},
      {"onTouchEventNative", "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z"},
      {"onKeyDownNative", "(JLandroid/view/KeyEvent;)Z"},
      {"onKeyUpNative", "(JLandroid/view/KeyEvent;)Z"},
  };
  for (const auto& requirement : kMvpRequirements) {
    if (!nuah_jvm_find_registered_native(jvm, kGameActivity,
                                         requirement.member,
                                         requirement.signature)) {
      throw std::runtime_error(
          "GameActivity initializeNativeCode did not register required "
          "callback " +
          std::string(requirement.member) + " " + requirement.signature);
    }
  }
  report_bootstrap_stage("GAMEACTIVITY_REGISTRATION_COMPLETE");
  report_bootstrap_stage("GAMEACTIVITY_START");
  if (!nuah_native_session_dispatch_lifecycle(session.get(), "onStartNative")) {
    throw std::runtime_error("GameActivity start callback is unavailable");
  }
  report_bootstrap_stage("GAMEACTIVITY_RESUME");
  if (!nuah_native_session_dispatch_lifecycle(session.get(), "onResumeNative")) {
    throw std::runtime_error("GameActivity resume callback is unavailable");
  }
  /* Android's window manager reports focus before it hands the native
   * activity its first Surface.  Keep that ordering: GameActivity queues
   * APP_CMD_GAINED_FOCUS here, and the Surface callbacks below then queue
   * APP_CMD_INIT_WINDOW with hasFocus already true. */
  if (!std::getenv("NUAH_DISABLE_DELAYED_FOCUS")) {
    unsigned long focus_delay_ms = 0;
    if (const char* value = std::getenv("NUAH_PRESTART_FOCUS_DELAY_MS");
        value && *value) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(value, &end, 10);
      if (end != value) focus_delay_ms = parsed;
    }
    if (focus_delay_ms != 0) ::usleep(focus_delay_ms * 1000U);
    report_bootstrap_stage("GAMEACTIVITY_FOCUS");
    const bool focus_ok =
        nuah_native_session_dispatch_window_focus(session.get(), 1) != 0;
    if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::cerr << "nuah native: activity focus-before-surface status="
                << (focus_ok ? 1 : 0) << '\n';
    }
  }
  const auto* native_window = nuah_window_session_native_window(window.get());
  const int width = nuah_native_window_width(native_window);
  const int height = nuah_native_window_height(native_window);
  if (!nuah_native_session_dispatch_surface_created(session.get(), surface) ||
      !nuah_native_session_dispatch_surface_changed(session.get(), surface, 0,
                                                    width, height)) {
    throw std::runtime_error("GameActivity surface lifecycle callback is unavailable");
  }
  report_bootstrap_stage("GRAPHICS_LIFECYCLE_ACTIVE");
  nuah_roblox_java_facade_set_launch_surface(
      reinterpret_cast<jobject>(surface));
  const bool async_game_start = [] {
    const char* value = std::getenv("NUAH_ASYNC_GAME_START");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  if (options.uri && !options.uri->empty()) {
    const auto request = parse_roblox_uri(*options.uri);
    nuah_roblox_java_facade_set_launch_place_id(
        static_cast<jlong>(std::stoll(request.place_id)));
    const std::string request_access_code =
        request.game_instance_id.value_or("");
    const std::string request_reserved_server_access_code =
        request.reserved_server_access_code.value_or("");
    const jint request_join_type =
        nuah_roblox_user_id() > 0 && request_access_code.empty() ? 1 : 2;
    nuah_roblox_java_facade_set_start_game_params(
        request_access_code.c_str(), request_reserved_server_access_code.c_str(),
        nuah_roblox_user_id(), request_join_type);
    const auto start_game = reinterpret_cast<jint (*)(JNIEnv*, jclass, jobject)>(
        image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_"
                     "nativeAppBridgeV2StartGameWithParam"));
    using StartApp = void (*)(JNIEnv*, jclass, jobject);
    const auto start_app = reinterpret_cast<StartApp>(
        image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_"
                     "nativeAppBridgeV2StartAppWithParams"));
    using StartLuaApp = void (*)(JNIEnv*, jclass);
    const auto start_lua_app = reinterpret_cast<StartLuaApp>(image.symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_"
        "nativeAppBridgeStartLuaAppDM"));
    using UpdateSurfaceGame = void (*)(JNIEnv*, jclass, jobject, jobject,
                                       jobject);
    const auto update_surface_game = reinterpret_cast<UpdateSurfaceGame>(
        image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_"
                     "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams"));
    const auto start_params = make_real_start_game_params(
        env, reinterpret_cast<jobject>(surface),
        reinterpret_cast<jobject>(nuah_jvm_game_activity(jvm)), request,
        content_path.c_str());
    trace_start_game_params(env, start_params);
    const auto activity = reinterpret_cast<jobject>(nuah_jvm_game_activity(jvm));
    const auto platform_params =
        make_real_platform_params(env, content_path.c_str());
    const bool start_app_with_params = [] {
      const char* value = std::getenv("NUAH_START_APP_WITH_PARAMS");
      return value && *value && std::strcmp(value, "0") != 0;
    }();
    const auto start_app_params = start_app_with_params
                                      ? make_real_start_app_params(
                                            env, reinterpret_cast<jobject>(surface),
                                            platform_params)
                                      : nullptr;
    if (!native_gl || !start_game || !start_params || !update_surface_game ||
        !activity || !platform_params ||
        (start_app_with_params && (!start_app || !start_app_params))) {
      throw std::runtime_error(
          "Roblox direct game-start JNI contract is unavailable");
    }
    nuah_window_session_show_loading(window.get());
    const bool start_lua_app_dm = [] {
      const char* value = std::getenv("NUAH_START_LUA_APP_DM");
      return value && *value && std::strcmp(value, "0") != 0;
    }();
    if (start_lua_app_dm && start_lua_app) {
      /* This is the exact SurfaceView-ready handoff used by ATL's launcher:
       * it creates SingleSurfaceApp/Lua state before the UGC join.  Without
       * it Roblox accepts the place request but later reports that
       * SingleSurfaceApp is uninitialized and leaves a ghost window. */
      report_bootstrap_stage("ROBLOX_APP_BRIDGE_START_LUA");
      start_lua_app(env, native_gl);
      clear_java_exception(env, "nativeAppBridgeStartLuaAppDM");
      if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::cerr << "nuah native: nativeAppBridgeStartLuaAppDM invoked\n";
      }
    }
    /* A real Android SurfaceView can publish its first size before the
     * activity's join call returns.  Some Roblox builds only create their
     * UGC render session when that callback is already pending.  Keep this
     * ordering as an explicit A/B switch: it is safe to disable while
     * comparing the direct native path, and avoids calling Roblox's private
     * updateSurface helper after the session has started. */
    const bool surface_view_callback_before_start = [] {
      const char* value = std::getenv("NUAH_SURFACE_VIEW_CALLBACK_BEFORE_START");
      return value && *value && std::strcmp(value, "0") != 0;
    }();
    if (surface_view_callback_before_start) {
      report_bootstrap_stage("ROBLOX_GAME_SURFACE_VIEW_CALLBACK_PRESTART");
      const int queued = nuah_jvm_dispatch_surface_view_lifecycle(
          jvm, nuah_native_window_width(native_window),
          nuah_native_window_height(native_window));
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::cerr << "nuah native: prestart SurfaceView callback queued="
                  << queued << '\n';
      }
    }
    if (start_app_with_params) {
      /* AppShellManager.startApp() is the missing bridge in the direct
       * launcher.  It initializes SingleSurfaceApp with the real Surface and
       * PlatformParams before the game join; without it Roblox can report a
       * successful UGC transition while leaving a ghost window. */
      report_bootstrap_stage("ROBLOX_APP_START_WITH_PARAMS");
      start_app(env, native_gl, start_app_params);
      clear_java_exception(env, "nativeAppBridgeV2StartAppWithParams");
      if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::cerr << "nuah native: nativeAppBridgeV2StartAppWithParams invoked\n";
      }
    }
    report_bootstrap_stage("ROBLOX_GAME_START");
    /* App/bootstrap initialization can rebuild the native HTTP client after
     * the early activity setup. Re-apply the authenticated Sober header at
     * the same boundary immediately before the join request. */
    prime_roblox_cookie_store(env, false);
    jint start_result = 0;
    if (!async_game_start) {
      start_result = start_game(env, native_gl, start_params);
    } else {
      /* The direct bridge entry can perform the first render transition on
       * its caller thread and block until the window-focus transition that
       * Android normally delivers from the UI loop. Run that entry on an
       * attached ART thread so this launch thread can deliver focus at the
       * same point as the working ATL launcher. */
      JavaVM* vm = reinterpret_cast<JavaVM*>(nuah_jvm_java_vm(jvm));
      struct AsyncStartState {
        JavaVM* vm = nullptr;
        jobject native_gl = nullptr;
        jobject start_params = nullptr;
        jint (*start)(JNIEnv*, jclass, jobject) = nullptr;
        std::atomic<jint> result{-1};
        std::atomic<bool> done{false};
        std::atomic<bool> refs_deleted{false};
      };
      auto async_state = std::make_shared<AsyncStartState>();
      async_state->vm = vm;
      async_state->native_gl = env->NewGlobalRef(native_gl);
      async_state->start_params = env->NewGlobalRef(start_params);
      async_state->start = start_game;
      std::thread starter([async_state] {
        JNIEnv* worker_env = nullptr;
        if (!async_state->vm ||
            async_state->vm->AttachCurrentThread(&worker_env, nullptr) != JNI_OK ||
            !worker_env) {
          async_state->done.store(true, std::memory_order_release);
          return;
        }
        async_state->result.store(
            async_state->start(
                worker_env, reinterpret_cast<jclass>(async_state->native_gl),
                async_state->start_params),
            std::memory_order_release);
        worker_env->DeleteGlobalRef(async_state->native_gl);
        worker_env->DeleteGlobalRef(async_state->start_params);
        async_state->refs_deleted.store(true, std::memory_order_release);
        async_state->done.store(true, std::memory_order_release);
        async_state->vm->DetachCurrentThread();
      });
      unsigned long focus_delay_ms = 500;
      if (const char* value = std::getenv("NUAH_ASYNC_FOCUS_DELAY_MS");
          value && *value) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value) focus_delay_ms = parsed;
      }
      if (focus_delay_ms != 0) ::usleep(focus_delay_ms * 1000U);
      if (!std::getenv("NUAH_DISABLE_DELAYED_FOCUS")) {
        report_bootstrap_stage("GAMEACTIVITY_FOCUS");
        const bool focus_ok =
            nuah_native_session_dispatch_window_focus(session.get(), 1) != 0;
        if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
            trace && *trace) {
          std::cerr << "nuah native: async window focus hasFocus=true status="
                    << (focus_ok ? 1 : 0) << '\n';
        }
      }
      unsigned long start_timeout_ms = 15000;
      if (const char* value = std::getenv("NUAH_ASYNC_START_TIMEOUT_MS");
          value && *value) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value) start_timeout_ms = parsed;
      }
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(start_timeout_ms);
      while (!async_state->done.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        ::usleep(1000);
      }
      if (!async_state->done.load(std::memory_order_acquire)) {
        std::cerr << "nuah native: async Roblox start timed out after "
                  << start_timeout_ms << " ms\n";
        /* The JNI entry has no cancellation contract.  Detach the worker so
         * a blocked Roblox call cannot freeze the supervisor; the shared
         * state keeps its global references alive until the worker returns. */
        starter.detach();
        start_result = -ETIMEDOUT;
      } else {
        starter.join();
        if (!async_state->refs_deleted.exchange(true,
                                                std::memory_order_acq_rel)) {
          env->DeleteGlobalRef(async_state->native_gl);
          env->DeleteGlobalRef(async_state->start_params);
        }
        start_result = async_state->result.load(std::memory_order_acquire);
      }
    }
    /* ExperienceSession starts its data-model work asynchronously.  On
     * Android the next SurfaceHolder callback arrives after the UI returns
     * to the looper; calling updateSurface in the same native stack frame
     * races that initialization and leaves the render-session object null.
     * Give the worker a small, configurable handoff window before matching
     * that later callback. */
    unsigned long settle_ms = 500;
    if (const char* value = ::getenv("NUAH_GAME_START_SETTLE_MS");
        value && *value) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(value, &end, 10);
      if (end != value) settle_ms = parsed;
    }
    if (settle_ms != 0) ::usleep(settle_ms * 1000U);
    /* nativeAppBridgeV2StartGameWithParam already schedules Roblox's own
     * UGC-game surface transition.  Calling updateSurface again too early
     * races that worker: the first EGL context is rebuilt and the second call
     * can divide by a zero-sized framebuffer.  Let Roblox's callback own the
     * playable path; set NUAH_EXPLICIT_SURFACE_UPDATE=1 only for an ABI
     * comparison run. */
    const bool explicit_surface_update = [] {
      const char* value = ::getenv("NUAH_EXPLICIT_SURFACE_UPDATE");
      return value && *value && std::strcmp(value, "0") != 0;
    }();
    const bool callback_surface_update = [] {
      const char* value = ::getenv("NUAH_SURFACE_CALLBACK_UPDATE");
      return value && *value && std::strcmp(value, "0") != 0;
    }();
    const bool surface_view_callback = [] {
      const char* value = ::getenv("NUAH_SURFACE_VIEW_CALLBACK");
      return value && *value && std::strcmp(value, "0") != 0;
    }();
    if (surface_view_callback) {
      /* Older installed ATL native providers do not emit their C-side
       * SurfaceView replay hook, even though the Java façade implements the
       * real SurfaceHolder callback.  Queue that Java callback directly on
       * the provider's UI loop; MainGameActivity then performs the same
       * private surface update Android would have delivered. */
      report_bootstrap_stage("ROBLOX_GAME_SURFACE_VIEW_CALLBACK");
      const int queued = nuah_jvm_dispatch_surface_view_lifecycle(
          jvm, nuah_native_window_width(native_window),
          nuah_native_window_height(native_window));
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::cerr << "nuah native: SurfaceView callback queued=" << queued
                  << '\n';
      }
    }
    if (const char* focus_after_start =
            std::getenv("NUAH_FOCUS_AFTER_START");
        focus_after_start && *focus_after_start &&
            std::strcmp(focus_after_start, "0") != 0) {
      /* Android reports the first window focus after SurfaceHolder has been
       * delivered.  This opt-in ordering reproduces that state machine for
       * launch paths that deliberately suppress the synthetic pre-surface
       * focus above. */
      report_bootstrap_stage("GAMEACTIVITY_FOCUS_AFTER_START");
      const bool focus_ok =
          nuah_native_session_dispatch_window_focus(session.get(), 1) != 0;
      if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::cerr << "nuah native: post-start focus status="
                  << (focus_ok ? 1 : 0) << '\n';
      }
    }
    if (callback_surface_update) {
      /* Android delivers this through SurfaceView/Choreographer after the
       * UI thread returns from the join call. Re-enter the registered
       * GameActivity callback rather than calling Roblox's private update
       * helper directly; the latter assumes an already-created UGC render
       * session and can dereference a null session on direct launch. */
      report_bootstrap_stage("ROBLOX_GAME_SURFACE_CALLBACK");
      if (!nuah_native_session_dispatch_surface_changed(
              session.get(), surface, 0,
              nuah_native_window_width(native_window),
              nuah_native_window_height(native_window))) {
        throw std::runtime_error(
            "GameActivity surface callback update was unavailable");
      }
      if (const char* focus_after = ::getenv("NUAH_FOCUS_AFTER_SURFACE");
          focus_after && *focus_after && std::strcmp(focus_after, "0") != 0) {
        report_bootstrap_stage("GAMEACTIVITY_FOCUS");
        (void)nuah_native_session_dispatch_window_focus(session.get(), 1);
      }
    } else if (explicit_surface_update) {
      report_bootstrap_stage("ROBLOX_GAME_SURFACE_UPDATE");
      update_surface_game(env, native_gl, reinterpret_cast<jobject>(surface),
                          platform_params, activity);
      /* On Android the window-manager focus notification follows the
       * SurfaceHolder update.  Keep this opt-in hook for the direct native
       * launcher so a room join can create its NetworkClient before Roblox's
       * first render-job resize. */
      if (const char* focus_after = ::getenv("NUAH_FOCUS_AFTER_SURFACE");
          focus_after && *focus_after && std::strcmp(focus_after, "0") != 0) {
        report_bootstrap_stage("GAMEACTIVITY_FOCUS");
        (void)nuah_native_session_dispatch_window_focus(session.get(), 1);
      }
    } else if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
               trace && *trace) {
      std::cerr << "nuah native: Roblox-owned UGC surface transition\n";
    }
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
      std::cerr << "nuah native: nativeAppBridgeV2StartGameWithParam placeId="
                << request.place_id << " status=" << start_result << '\n';
    }
  }
  nuah_input_bind_native_session(session.get());
  const unsigned int input_sleep_us = input_poll_sleep_us();
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace) {
    std::cerr << "nuah input: host poll interval=" << input_sleep_us
              << "us\n";
  }
  while (!nuah_window_session_should_close(window.get())) {
    nuah_window_session_pump(window.get());
    (void)nuah_input_pump();
    if (input_sleep_us != 0) ::usleep(input_sleep_us);
  }
  nuah_input_bind_native_session(nullptr);
  (void)nuah_native_session_dispatch_surface_destroyed(session.get(), surface);
  (void)nuah_native_session_dispatch_lifecycle(session.get(), "onPauseNative");
  (void)nuah_native_session_dispatch_lifecycle(session.get(), "onStopNative");
  nuah_native_session_clear_surface(session.get());
  std::cerr << "nuah native: libroblox.so retained through ATL's single JNI_OnLoad path\n";
  return 0;
}

/* Keep the Android-facing bootstrap on a dedicated host stack, but create
 * that thread only after the isolated child exists.  Forking from a worker
 * thread copies the parent's libc/loader lock state into the child; Roblox's
 * JNI_OnLoad then exercises that half-copied state through its TLS allocator.
 * A fork-first child has one clean host TLS domain before any Android DSO is
 * loaded. */
struct NativeIsolationThreadArgs {
  const NativeLaunchOptions* options;
  const std::filesystem::path* apk;
  int result;
  std::string error;
};

/* `timeout`, a terminal, or the desktop launcher may signal the native-run
 * parent while it is waiting for the isolated Roblox child.  Forward that
 * signal explicitly; relying only on PR_SET_PDEATHSIG leaves a small fork
 * race in which the child can be reparented before it installs the death
 * signal and keep the SQLite cache locked. */
volatile sig_atomic_t g_isolated_child_pid = -1;

void forward_isolated_child_signal(int signal_number) noexcept {
  const pid_t child = static_cast<pid_t>(g_isolated_child_pid);
  if (child > 0) (void)::kill(child, signal_number);
}

void* run_nuah_jni_isolated_on_large_stack(void* opaque) noexcept {
  auto* args = static_cast<NativeIsolationThreadArgs*>(opaque);
  try {
    args->result = run_nuah_jni(*args->options, *args->apk);
  } catch (const std::exception& error) {
    args->error = error.what();
    args->result = 1;
  } catch (...) {
    args->error = "native bootstrap failed with an unknown error";
    args->result = 1;
  }
  return nullptr;
}

int run_nuah_jni_isolated_impl(const NativeLaunchOptions& options,
                               const std::filesystem::path& apk) {
  // Android constructors run while android_dlopen is still active. Isolate
  // them so a native abort is converted into a useful launch error rather
  // than taking down the supervisor with only a core-dump message.
  void* mapping =
      ::mmap(nullptr, sizeof(NuahBootstrapDiagnostics),
             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    throw std::runtime_error("cannot create native bootstrap diagnostics");
  }
  auto* diagnostics = static_cast<NuahBootstrapDiagnostics*>(mapping);
  std::memset(diagnostics, 0, sizeof(*diagnostics));
  diagnostics->version = 1;
  nuah_bootstrap_diagnostics_attach(diagnostics);
  const pid_t parent_pid = ::getpid();
  const pid_t child = ::fork();
  if (child < 0) {
    nuah_bootstrap_diagnostics_attach(nullptr);
    ::munmap(mapping, sizeof(*diagnostics));
    throw std::runtime_error("cannot start isolated native bootstrap");
  }
  if (child == 0) {
#ifdef __linux__
    /* Do not leave a detached ART/Roblox child behind when the Nuah launcher
     * is closed.  The old process split was the reason stale 1818 windows and
     * locked databases survived later launches. */
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != parent_pid)
      _exit(70);
#endif
    if (!::getenv("NUAH_DISABLE_CRASH_HANDLER")) {
      nuah_bootstrap_diagnostics_install_signal_handler();
    }
    constexpr std::size_t kNativeBootstrapStack = 256u * 1024u * 1024u;
    pthread_attr_t attributes{};
    if (::pthread_attr_init(&attributes) != 0 ||
        ::pthread_attr_setstacksize(&attributes, kNativeBootstrapStack) != 0) {
      (void)::pthread_attr_destroy(&attributes);
      std::cerr << "nuah bootstrap: cannot allocate native bootstrap stack\n";
      _exit(70);
    }
    NativeIsolationThreadArgs arguments{&options, &apk, 1, {}};
    pthread_t worker{};
    const int create_result = ::pthread_create(
        &worker, &attributes, run_nuah_jni_isolated_on_large_stack, &arguments);
    (void)::pthread_attr_destroy(&attributes);
    if (create_result != 0) {
      std::cerr << "nuah bootstrap: cannot start native bootstrap thread (errno="
                << create_result << ")\n";
      _exit(70);
    }
    (void)::pthread_join(worker, nullptr);
    if (!arguments.error.empty()) {
      std::cerr << "nuah bootstrap: native initialization failed before JNI_OnLoad: "
                << arguments.error << '\n';
      _exit(70);
    }
    _exit(arguments.result == 0 ? 0 : 70);
  }
  struct sigaction forward_action {};
  struct sigaction previous_term {};
  struct sigaction previous_int {};
  forward_action.sa_handler = forward_isolated_child_signal;
  sigemptyset(&forward_action.sa_mask);
  forward_action.sa_flags = 0;
  g_isolated_child_pid = child;
  const bool term_handler_installed =
      ::sigaction(SIGTERM, &forward_action, &previous_term) == 0;
  const bool int_handler_installed =
      ::sigaction(SIGINT, &forward_action, &previous_int) == 0;
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  g_isolated_child_pid = -1;
  if (term_handler_installed) (void)::sigaction(SIGTERM, &previous_term, nullptr);
  if (int_handler_installed) (void)::sigaction(SIGINT, &previous_int, nullptr);
  if (waited != child) {
    nuah_bootstrap_diagnostics_attach(nullptr);
    ::munmap(mapping, sizeof(*diagnostics));
    throw std::runtime_error("cannot wait for isolated native bootstrap");
  }
  const NuahBootstrapDiagnostics result = *diagnostics;
  nuah_bootstrap_diagnostics_attach(nullptr);
  ::munmap(mapping, sizeof(*diagnostics));
  if (result.module_path[0]) {
    const std::filesystem::path crash_module(result.module_path);
    if (crash_module.parent_path() == "/tmp" &&
        crash_module.filename().string().starts_with("nuah-module-")) {
      std::error_code ignored;
      std::filesystem::remove(crash_module, ignored);
    }
  }
  const std::string stage = result.stage[0] ? result.stage : "NO_STAGE";
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  if (WIFSIGNALED(status)) {
    std::ostringstream message;
    message << "native bootstrap terminated by signal " << WTERMSIG(status)
            << " at " << stage;
    if (result.abort_seen) {
      message << "; crash caller "
              << (result.module_path[0] ? result.module_path : "(unmapped)")
              << "+0x" << std::hex << result.module_offset << std::dec
              << " (pc=0x" << std::hex << result.caller << std::dec
              << " tid=" << result.thread_id << ")";
      if (result.parent_module_offset) {
        message << "; parent module offset +0x" << std::hex
                << result.parent_module_offset << std::dec;
      }
    }
    if (result.fault_address) {
      message << "; fault-address=0x" << std::hex << result.fault_address
              << std::dec;
    }
    if (result.abort_message[0]) {
      message << "; Android abort message: " << result.abort_message;
    }
    if (result.last_log[0]) {
      message << "; last Android log: " << result.last_log;
    }
    if (result.last_property[0]) {
      message << "; last Android property: " << result.last_property;
    }
    throw std::runtime_error(message.str());
  }
  throw std::runtime_error("native bootstrap exited with status " +
                           std::to_string(WEXITSTATUS(status)) +
                           " at " + stage);
}

int run_nuah_jni_isolated(const NativeLaunchOptions& options,
                          const std::filesystem::path& apk) {
  /* The parent remains single-threaded for native-run.  Forking here, before
   * the Android linker/SDL providers are loaded, avoids inheriting locks from
   * a host worker; the child creates its large-stack bootstrap thread above. */
  return run_nuah_jni_isolated_impl(options, apk);
}

int run_bionic_loader(const std::filesystem::path& image) {
  const auto root = runtime_directory();
  const auto linker = root / "bionic/lib64/linker64";
  const auto helper = root / "bionic/nuah-bionic-loader";
  // This namespace must contain Android ELF only.  In particular, do not add
  // Nuah's host-glibc android/ providers here: linker64 cannot load them.
  const auto library_path = (root / "bionic/lib64").string();
  if (!std::filesystem::is_regular_file(linker) ||
      !std::filesystem::is_regular_file(helper)) {
    throw std::runtime_error("Nuah bionic runtime bundle is missing linker64 or helper");
  }
  const auto child = ::fork();
  if (child < 0) throw std::runtime_error("cannot start bionic loader helper");
  if (child == 0) {
    // Android linker64's direct-exec mode takes an absolute program path as
    // argv[1]; configure its lookup path through the normal linker variable.
    ::setenv("LD_LIBRARY_PATH", library_path.c_str(), 1);
    ::execl(linker.c_str(), linker.c_str(), helper.c_str(), image.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child) {
    throw std::runtime_error("cannot wait for bionic loader helper");
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  if (WIFEXITED(status)) {
    throw std::runtime_error("bionic loader helper exited with status " +
                             std::to_string(WEXITSTATUS(status)));
  }
  throw std::runtime_error("bionic loader helper terminated by signal " +
                           std::to_string(WTERMSIG(status)));
}

}  // namespace

int run_native(const NativeLaunchOptions& options) {
  NativeLaunchOptions launch = options;
  /* On the Intel UHD 620, the 1280x720 Android surface is GPU-bound: the
   * same Vulkan path holds 60Hz at 960x540 but only 40--48Hz at 1280x720.
   * Keep the higher resolution available explicitly while making the normal
   * native MVP usable on modest integrated GPUs. */
  const char* performance_mode = std::getenv("NUAH_PERFORMANCE_MODE");
  const bool quality_mode =
      performance_mode &&
      (std::strcmp(performance_mode, "quality") == 0 ||
       std::strcmp(performance_mode, "full") == 0);
  /* Match the settings path above: absent mode means turbo.  Balanced/low
   * and quality/full remain explicit opt-outs for larger surfaces. */
  const bool turbo_mode =
      (!performance_mode || !*performance_mode ||
       std::strcmp(performance_mode, "turbo") == 0 ||
       std::strcmp(performance_mode, "fast") == 0);
  if (!quality_mode && !launch.dimensions_explicit && launch.width == 1280 &&
      launch.height == 720) {
    if (turbo_mode) {
      launch.width = 720;
      launch.height = 405;
      std::cerr << "nuah performance: turbo Vulkan surface 720x405 at 60 FPS "
                   "(use NUAH_TARGET_FPS for a high-refresh host, or "
                   "--width/--height or NUAH_PERFORMANCE_MODE=balanced "
                   "for the larger 960x540 profile)\n";
    } else {
      launch.width = 960;
      launch.height = 540;
      std::cerr << "nuah performance: balanced Vulkan surface 960x540 "
                   "(use --width 1280 --height 720 or "
                   "NUAH_PERFORMANCE_MODE=quality for full resolution)\n";
    }
  }
  if (!std::filesystem::is_regular_file(launch.apk)) {
    throw std::runtime_error("native APK does not exist: " +
                             launch.apk.string());
  }
  if (launch.width <= 0 || launch.height <= 0) {
    throw std::runtime_error("native window dimensions must be positive");
  }
  const auto runtime_data_directory = std::filesystem::absolute(
      launch.data_directory.value_or(std::filesystem::temp_directory_path() /
                                     "nuah-data"));
  /* TMPDIR is redirected to the profile below.  Pin the resolved profile in
   * the launch options before that change so run_nuah_jni and
   * prepare_atl_native_libraries cannot accidentally nest it under TMPDIR. */
  if (!launch.data_directory) launch.data_directory = runtime_data_directory;
  RuntimeDataLock runtime_data_lock(runtime_data_directory);
  /* Recover a stale AssetProvider WAL while the profile lock is held, before
   * ART can open the database on a render-adjacent worker. */
  checkpoint_asset_cache(runtime_data_directory, launch.apk);

  /* Keep ART/Roblox temporary files beside the app profile.  The default
   * TMPDIR on a desktop Linux session is usually a small tmpfs; Roblox's
   * AssetProvider can have several KTX2/Basis responses in flight and its
   * cache writer reports those short writes as HttpError::OutOfMemory when
   * that tmpfs fills.  Nuah owns a persistent per-profile directory already,
   * so use it for temporary downloads, dex scratch files, and decompression
   * spill.  An explicit NUAH_TMPDIR remains available for diagnostics.
   */
  const auto profile_tmp = runtime_data_directory / "tmp";
  const char* requested_tmp = std::getenv("NUAH_TMPDIR");
  const std::filesystem::path temporary_directory =
      requested_tmp && *requested_tmp ? std::filesystem::path(requested_tmp)
                                      : profile_tmp;
  std::error_code temporary_error;
  std::filesystem::create_directories(temporary_directory, temporary_error);
  if (temporary_error ||
      ::setenv("TMPDIR", temporary_directory.c_str(), 1) != 0) {
    throw std::runtime_error("cannot configure Nuah profile temporary directory");
  }
  if (const char* trace = std::getenv("NUAH_BOOTSTRAP_TRACE");
      trace && *trace) {
    std::cerr << "nuah runtime: temporary directory="
              << temporary_directory << '\n';
  }

  /* ATL's monitor callback reports the physical desktop to Android's
   * Configuration.  That is correct for a conventional ATL window, but the
   * native launcher deliberately chooses a smaller render surface on modest
   * integrated GPUs.  Without this handoff Roblox accepts the selected size
   * at startup and then rebuilds its swapchain at the desktop resolution when
   * the room changes orientation.  Make the host-selected dimensions the
   * normal native contract; an explicit NUAH_LOCK_SURFACE_SIZE=0 retains the
   * upstream monitor-sized behaviour for diagnostics. */
  const char* surface_lock = ::getenv("NUAH_LOCK_SURFACE_SIZE");
  if (!surface_lock || std::strcmp(surface_lock, "0") != 0) {
    const std::string width = std::to_string(launch.width);
    const std::string height = std::to_string(launch.height);
    if (::setenv("NUAH_LOCK_SURFACE_SIZE", "1", 1) != 0 ||
        ::setenv("NUAH_SURFACE_WIDTH", width.c_str(), 1) != 0 ||
        ::setenv("NUAH_SURFACE_HEIGHT", height.c_str(), 1) != 0) {
      throw std::runtime_error("cannot configure native Android surface size");
    }
  }

  if (!std::getenv("NUAH_DISABLE_SESSION_DISCOVERY")) {
    (void)discover_sober_session_cookie();
    (void)discover_webkit_user_id();
  }

  const auto image_apk = find_image(launch);
  if (const char* smoke = ::getenv("NUAH_NATIVE_BIONIC_SMOKE"); smoke && *smoke) {
    const auto image = extract_roblox_image(image_apk);
    try {
      run_bionic_loader(image);
    } catch (...) {
      std::error_code ignored;
      std::filesystem::remove(image, ignored);
      throw;
    }
    std::error_code ignored;
    std::filesystem::remove(image, ignored);
    std::cerr << "nuah native: API-36 bionic loader accepted libroblox.so from "
              << image_apk << '\n';
    return 0;
  }
  try {
    const int result = run_nuah_jni_isolated(launch, image_apk);
    /* The isolated child has exited, so no Android SQLite handle remains.
     * Compact the cache now as well as at the next preflight; this keeps a
     * normal close from handing a multi-megabyte WAL to the next launch. */
    checkpoint_asset_cache(runtime_data_directory, launch.apk);
    return result;
  } catch (...) {
    /* A crash/abort can still leave a recoverable WAL. The lock is held and
     * the child has been reaped by run_nuah_jni_isolated, so best-effort
     * checkpointing is safe before propagating the original failure. */
    checkpoint_asset_cache(runtime_data_directory, launch.apk);
    throw;
  }
}

}  // namespace nuah
