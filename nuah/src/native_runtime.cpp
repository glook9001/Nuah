#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include "nuah/apk_loader.hpp"
#include "nuah/bootstrap_diagnostics.h"
#include "nuah/input_bridge.h"
#include "nuah/launch_uri.hpp"
#include "nuah/native_backend.hpp"
#include "nuah/native_session.h"
#include "nuah/nuah_jvm.h"
#include "nuah/window_session.h"
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <jni.h>
#include <signal.h>
#include <unistd.h>
#include <vector>
namespace nuah {
namespace {
extern "C" void nuah_roblox_java_facade_set_content_path(const char* path);
extern "C" void nuah_roblox_java_facade_set_launch_place_id(jlong place_id);
extern "C" void nuah_roblox_java_facade_set_start_game_params(const char* access_code, const char* reserved_server_access_code, jlong user_id, jint join_request_type);
extern "C" void nuah_roblox_java_facade_set_launch_surface(jobject surface);
using RobloxSetMultipleCookies = void (*)(JNIEnv*, jclass, jstring, jstring);
using RobloxGetCookiesForDomain = jstring (*)(JNIEnv*, jclass, jstring);
RobloxSetMultipleCookies g_roblox_set_multiple_cookies = nullptr;
RobloxGetCookiesForDomain g_roblox_get_cookies_for_domain = nullptr;
void report_bootstrap_stage(const char* stage) { nuah_bootstrap_diagnostics_set_stage(stage); }
static inline const char* nonempty_env(const char* n) { const char* v = ::getenv(n); return v && *v ? v : nullptr; }
static inline const char* first_nonempty_env(const char* a, const char* b = nullptr, const char* c = nullptr) { for (auto n : {a, b, c}) if (n) if (const char* v = nonempty_env(n)) return v; return nullptr; }
static inline bool is_regular_file_ok(const std::filesystem::path& p) { std::error_code e; const auto s = std::filesystem::status(p, e); return !e && std::filesystem::is_regular_file(s); }
static inline std::string_view rbxuid_token(std::string_view s) { constexpr std::string_view k = "rbxuid="; const size_t p = s.find(k); if (p == std::string_view::npos) return {}; size_t e = p + k.size(); while (e < s.size() && s[e] >= '0' && s[e] <= '9') ++e; return s.substr(p + k.size(), e - p - k.size()); }
static inline jlong parse_user_id_str(const char* s) { char* e = nullptr; const long long v = (s && *s) ? std::strtoll(s, &e, 10) : 0; return (s && *s && e != s && e && *e == '\0' && v > 0) ? static_cast<jlong>(v) : 0; }
static inline unsigned long parse_ulong_env(const char* name, unsigned long def, unsigned long min_val, unsigned long max_val, bool* is_explicit);
static inline bool parse_bool_env(const char* name, bool def, bool* is_explicit);
unsigned int input_poll_sleep_us() {
  static const unsigned int value = static_cast<unsigned int>(parse_ulong_env("NUAH_INPUT_POLL_SLEEP_US", 2000, 0, 20000, nullptr));
  return value;
}
bool fast_mvp_enabled() { return parse_bool_env("NUAH_FAST_MVP", true, nullptr); }
inline bool bootstrap_trace() { return nonempty_env("NUAH_BOOTSTRAP_TRACE") != nullptr; }
void clear_java_exception(JNIEnv* env, const char* boundary) {
  if (!env || !env->ExceptionCheck()) return;
  if (bootstrap_trace()) {
    std::cerr << "nuah native: Java exception at " << boundary << '\n';
    env->ExceptionDescribe();
  }
  env->ExceptionClear();
}
static inline jobject fail_null(JNIEnv* env, const char* w) { clear_java_exception(env, w); return nullptr; }
static inline bool fail_false(JNIEnv* env, const char* w) { clear_java_exception(env, w); return false; }
bool asset_tree_has_files(const std::filesystem::path& root) {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) return false;
  std::filesystem::recursive_directory_iterator entries(root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (entries != end) {
    if (!error && std::filesystem::is_regular_file(entries->path(), error)) return true;
    error.clear();
    entries.increment(error);
  }
  return false;
}
void prefetch_local_asset_pages(const std::array<std::filesystem::path, 2>& roots) {
  if (!parse_bool_env("NUAH_ASSET_PREFETCH", true, nullptr)) return;
  const unsigned long budget_mb = parse_ulong_env("NUAH_ASSET_PREFETCH_MB", 512, 0, 4096, nullptr);
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
    std::filesystem::recursive_directory_iterator entries(root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; entries != end && requested < budget; entries.increment(error)) {
      if (error) {
        error.clear();
        continue;
      }
      std::error_code file_error;
      if (!std::filesystem::is_regular_file(entries->path(), file_error) || file_error) continue;
      const uintmax_t size = std::filesystem::file_size(entries->path(), file_error);
      if (file_error || size == 0) continue;
      const uint64_t remaining = budget - requested;
      const off_t hint_size = static_cast<off_t>(std::min<uint64_t>(remaining, static_cast<uint64_t>(size)));
      const int fd = ::open(entries->path().c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) continue;
      (void)::posix_fadvise(fd, 0, hint_size, POSIX_FADV_WILLNEED);
      (void)::close(fd);
      requested += static_cast<uint64_t>(hint_size);
      ++files;
    }
  }
  if (bootstrap_trace()) {
    std::cerr << "nuah assets: prefetch hints files=" << files << " bytes=" << requested << " budget=" << budget << '\n';
  }
}
std::string packaged_client_settings(const std::filesystem::path& app_data) {
  if (!parse_bool_env("NUAH_USE_PACKAGED_CLIENT_SETTINGS", false, nullptr)) return {};
  const std::array<std::filesystem::path, 3> roots = {app_data / "files/assets/shared_compression_dictionaries", app_data / "assets/shared_compression_dictionaries",
                                                      std::filesystem::path("/home/pepe/.local/share/nuah/base.apk_/") / "files/assets/shared_compression_dictionaries"};
  for (const auto& root : roots) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) continue;
    for (const auto& entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, error)) {
      if (error) break;
      if (!entry.is_regular_file(error) || error) {
        error.clear();
        continue;
      }
      if (entry.path().extension() != ".json") continue;
      std::ifstream input(entry.path(), std::ios::binary | std::ios::ate);
      if (!input) continue;
      const auto end = input.tellg();
      if (end <= 0 || end > std::streamoff(8 * 1024 * 1024)) continue;
      std::string contents(static_cast<std::size_t>(end), '\0');
      input.seekg(0, std::ios::beg);
      if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size()))) continue;
      const std::size_t first = contents.find_first_not_of(" \t\r\n");
      if (first == std::string::npos || contents[first] != '{') continue;
      if (contents.find("\"applicationSettings\"") == std::string::npos) continue;
      return contents;
    }
  }
  return {};
}
void set_client_setting(std::string& json, std::string_view key, std::string_view value) {
  const std::string needle = "\"" + std::string(key) + "\"";
  std::size_t key_pos = json.find(needle);
  if (key_pos != std::string::npos) {
    const std::size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string::npos) return;
    std::size_t begin = colon + 1;
    while (begin < json.size() && (json[begin] == ' ' || json[begin] == '\t' || json[begin] == '\r' || json[begin] == '\n')) { ++begin; }
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
      while (end > begin && (json[end - 1] == ' ' || json[end - 1] == '\t' || json[end - 1] == '\r' || json[end - 1] == '\n')) { --end; }
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
  if (!parse_bool_env("NUAH_SHADER_CACHE", true, nullptr)) return;
  std::filesystem::path directory = profile / "mesa-shader-cache";
  if (const char* requested = nonempty_env("NUAH_SHADER_CACHE_DIR")) { directory = requested; }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    if (bootstrap_trace()) { std::cerr << "nuah graphics: shader cache unavailable: " << error.message() << '\n'; }
    return;
  }
  if (!std::getenv("MESA_SHADER_CACHE_DIR")) (void)::setenv("MESA_SHADER_CACHE_DIR", directory.c_str(), 1);
  if (!std::getenv("MESA_SHADER_CACHE_MAX_SIZE")) {
    const char* requested = nonempty_env("NUAH_SHADER_CACHE_MAX_SIZE");
    (void)::setenv("MESA_SHADER_CACHE_MAX_SIZE", requested ? requested : "2G", 1);
  }
  if (!std::getenv("MESA_SHADER_CACHE_DISABLE")) (void)::setenv("MESA_SHADER_CACHE_DISABLE", "false", 1);
  if (!std::getenv("MESA_DISK_CACHE_SINGLE_FILE")) (void)::setenv("MESA_DISK_CACHE_SINGLE_FILE", "1", 1);
  if (!std::getenv("ANV_ENABLE_PIPELINE_CACHE")) (void)::setenv("ANV_ENABLE_PIPELINE_CACHE", "1", 1);
  if (bootstrap_trace()) std::cerr << "nuah graphics: Mesa shader cache=" << directory << '\n';
}
const char* engine_governor_profile() {
  const char* value = nonempty_env("NUAH_ENGINE_GOVERNOR");
  if (!value || std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0) return nullptr;
  if (std::strcmp(value, "balanced") == 0 || std::strcmp(value, "throughput") == 0) return value;
  if (bootstrap_trace()) std::cerr << "nuah governor: unknown profile '" << value << "' (use balanced, throughput, or off)\n";
  return nullptr;
}
void configure_mesa_submit_thread() {
  const char* requested = ::getenv("NUAH_VULKAN_SUBMIT_THREAD");
  const char* existing = ::getenv("MESA_VK_ENABLE_SUBMIT_THREAD");
  if (existing && (!requested || std::strcmp(requested, "auto") == 0)) return;
  const auto apply = [](const char* value) {
    if (::setenv("MESA_VK_ENABLE_SUBMIT_THREAD", value, 1) != 0) return;
    if (bootstrap_trace()) std::cerr << "nuah graphics: Mesa submit thread=" << (std::strcmp(value, "1") == 0 ? "enabled" : "disabled") << '\n';
  };
  if (requested && *requested && std::strcmp(requested, "auto") != 0) {
    if (std::strcmp(requested, "0") != 0 && std::strcmp(requested, "1") != 0) {
      if (bootstrap_trace()) { std::cerr << "nuah graphics: invalid NUAH_VULKAN_SUBMIT_THREAD='" << requested << "' (use auto, 0, or 1)\n"; }
      return;
    }
    apply(requested);
    return;
  }
  if (engine_governor_profile()) { apply("0"); return; }
  const unsigned int logical_cpus = std::thread::hardware_concurrency();
  if (logical_cpus == 0 || logical_cpus > 4) return;
  apply("0");
}
extern "C" jint nuah_native_get_running_architecture(JNIEnv*, jclass) { return 3; }
static inline std::string roblosecurity_token(std::string_view text, std::string_view delims = ";\t\r\n") {
  constexpr std::string_view kMarker = ".ROBLOSECURITY=";
  const std::size_t pos = text.find(kMarker);
  if (pos == std::string_view::npos) return {};
  const std::size_t begin = pos + kMarker.size(), end = text.find_first_of(delims, begin);
  return std::string(text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
}
static inline void extract_protobuf_string(std::string_view decoded, std::string_view key, const char* env_name, std::size_t max_len) {
  const std::size_t pos = decoded.find(key);
  if (pos == std::string_view::npos) return;
  std::size_t begin = pos + key.size();
  if (begin >= decoded.size()) return;
  const unsigned char len = static_cast<unsigned char>(decoded[begin++]);
  if (len == 0 || len > max_len || begin + len > decoded.size()) return;
  const std::string value(decoded.substr(begin, len));
  if (!value.empty()) (void)::setenv(env_name, value.c_str(), 1);
}
void extract_cookie_user_name(std::string_view cookie_value) {
  const std::size_t marker = cookie_value.find("|_");
  if (marker == std::string_view::npos) return;
  const std::size_t begin = marker + 2, end = cookie_value.find('.', begin);
  const std::string_view encoded = cookie_value.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
  if (encoded.empty() || encoded.size() > 4096) return;
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string decoded;
  int value = 0, bits = -8;
  for (const unsigned char c : encoded) {
    const char* found = std::strchr(alphabet, c);
    if (!found) break;
    value = (value << 6) + static_cast<int>(found - alphabet);
    bits += 6;
    if (bits >= 0) {
      decoded.push_back(static_cast<char>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  extract_protobuf_string(decoded, std::string_view("\x05uname\x12", 7), "NUAH_ROBLOX_USERNAME", 50); extract_protobuf_string(decoded, std::string_view("\x03uid\x12", 5), "NUAH_ROBLOX_USER_ID", 20);
}
static inline bool store_cookie_token(const std::string& v, std::string_view h, bool can_throw) {
  if (v.empty() || v.size() > 4096) return false;
  if (::setenv("NUAH_ROBLOX_COOKIES", (".ROBLOSECURITY=" + v).c_str(), 1) != 0 && can_throw) throw std::runtime_error("cannot import session cookie");
  extract_cookie_user_name(v);
  if (h.size() <= 16384 && h.find_first_of("\r\n") == std::string_view::npos) (void)::setenv("NUAH_ROBLOX_COOKIE_HEADER", std::string(h).c_str(), 1);
  return true;
}
std::string nuah_roblox_cookie_value() {
  if (const char* raw = first_nonempty_env("NUAH_ROBLOX_COOKIES", "ROBLOX_COOKIE", "NUAH_ROBLOX_COOKIE_HEADER")) {
    if (std::string_view(raw).find(".ROBLOSECURITY=") != std::string_view::npos) return roblosecurity_token(raw);
    return std::string(raw);
  }
  return {};
}
std::string nuah_roblox_cookie_header() {
  if (const char* header = nonempty_env("NUAH_ROBLOX_COOKIE_HEADER")) return header;
  const std::string value = nuah_roblox_cookie_value();
  return value.empty() ? std::string() : ".ROBLOSECURITY=" + value;
}
jlong nuah_roblox_user_id() {
  const char* raw = nonempty_env("NUAH_ROBLOX_USER_ID");
  if (!raw) {
    const std::string cookie_val = nuah_roblox_cookie_value();
    if (!cookie_val.empty()) {
      extract_cookie_user_name(cookie_val);
      raw = nonempty_env("NUAH_ROBLOX_USER_ID");
    }
  }
  return parse_user_id_str(raw);
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
void adopt_cookie_header(std::string_view header) { (void)store_cookie_token(roblosecurity_token(header), header, false); }
void prime_roblox_cookie_store(JNIEnv* env, bool early_bootstrap) {
  if (early_bootstrap) {
    const bool force_enabled = parse_bool_env("NUAH_EARLY_COOKIE_PRIME", false, nullptr), skip_enabled = parse_bool_env("NUAH_SKIP_COOKIE_PRIME", false, nullptr);
    if (!force_enabled || skip_enabled) {
      if (bootstrap_trace()) std::fprintf(stderr, "nuah cookie: skipping early native-store prime\n");
      return;
    }
  }
  if (!env || !g_roblox_set_multiple_cookies) return;
  const std::string cookie = nuah_roblox_cookie_header();
  if (cookie.empty()) return;
  if (bootstrap_trace()) { std::fprintf(stderr, "nuah cookie: native setter=%p header_bytes=%zu\n", reinterpret_cast<void*>(g_roblox_set_multiple_cookies), cookie.size()); }
  jstring url = env->NewStringUTF("https://roblox.com/");
  jstring header = env->NewStringUTF(cookie.c_str());
  if (!url || !header) { clear_java_exception(env, "NewStringUTF(cookie prime)"); if (url) env->DeleteLocalRef(url); if (header) env->DeleteLocalRef(header); return; }
  g_roblox_set_multiple_cookies(env, nullptr, url, header);
  if (env->ExceptionCheck()) clear_java_exception(env, "nativeSetMultipleCookies(prime)");
  if (bootstrap_trace() && g_roblox_get_cookies_for_domain) {
    jstring stored = g_roblox_get_cookies_for_domain(env, nullptr, url);
    const std::string stored_header = jstring_utf8(env, stored);
    if (stored) env->DeleteLocalRef(stored);
    std::cerr << "nuah cookie: Roblox native store header_bytes=" << stored_header.size() << '\n';
  }
  env->DeleteLocalRef(url);
  env->DeleteLocalRef(header);
  if (bootstrap_trace()) { std::cerr << "nuah cookie: primed native store header_bytes=" << cookie.size() << '\n'; }
}
bool discover_session_cookie() {
  if (const char* existing = nonempty_env("NUAH_ROBLOX_COOKIES")) {
    extract_cookie_user_name(existing);
    return true;
  }
  std::vector<std::filesystem::path> candidates;
  if (const char* v = nonempty_env("NUAH_DATA_DIR")) candidates.emplace_back(std::filesystem::path(v) / "cookies");
  if (const char* v = nonempty_env("XDG_DATA_HOME")) candidates.emplace_back(std::filesystem::path(v) / "nuah" / "cookies");
  if (const char* home = nonempty_env("HOME")) {
    const std::filesystem::path home_path(home);
    candidates.push_back(home_path / ".local/share/nuah/cookies");
  }
  for (const auto& path : candidates) {
    if (!is_regular_file_ok(path)) continue;
    std::ifstream file(path, std::ios::in | std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
      if (!store_cookie_token(roblosecurity_token(line), line, true)) continue;
      if (const auto id = rbxuid_token(line); !id.empty() && id.size() <= 20) (void)::setenv("NUAH_ROBLOX_USER_ID", std::string(id).c_str(), 1);
      std::cerr << "nuah native: adopted browser session from " << path << '\n';
      return true;
    }
  }
  return false;
}
extern "C" jstring nuah_native_get_cookies_netscape(JNIEnv* env, jclass, jstring) {
  const std::string header = nuah_roblox_cookie_header();
  if (bootstrap_trace()) { std::cerr << "nuah cookie: Netscape getter header_bytes=" << header.size() << '\n'; }
  if (!env || header.empty() || header.find_first_of("\r\n") != std::string::npos) { return env ? env->NewStringUTF("") : nullptr; }
  std::string netscape;
  std::size_t offset = 0;
  while (offset < header.size()) {
    const std::size_t next = header.find(';', offset);
    std::string_view pair(header.data() + offset, (next == std::string::npos ? header.size() : next) - offset);
    while (!pair.empty() && (pair.front() == ' ' || pair.front() == '\t')) { pair.remove_prefix(1); }
    const std::size_t equal = pair.find('=');
    if (equal != std::string_view::npos && equal > 0) {
      const std::string_view name = pair.substr(0, equal), value = pair.substr(equal + 1);
      if (!value.empty() && value.find_first_of("\r\n\t") == std::string_view::npos) {
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
extern "C" jstring nuah_native_get_cookies_for_domain(JNIEnv* env, jclass, jstring) {
  const std::string header = nuah_roblox_cookie_header();
  if (bootstrap_trace()) { std::cerr << "nuah cookie: domain getter header_bytes=" << header.size() << '\n'; }
  return env ? env->NewStringUTF(header.c_str()) : nullptr;
}
extern "C" void nuah_native_set_cookies(JNIEnv* env, jclass, jstring domain, jstring cookies) {
  const std::string domain_value = jstring_utf8(env, domain);
  const std::string cookie_value = jstring_utf8(env, cookies);
  adopt_cookie_header(cookie_value);
  const bool preserve_primed_session = cookie_value.empty() && !nuah_roblox_cookie_header().empty();
  if (g_roblox_set_multiple_cookies && !preserve_primed_session) { g_roblox_set_multiple_cookies(env, nullptr, domain, cookies); }
  if (bootstrap_trace()) {
    std::cerr << "nuah cookie: setter domain_bytes=" << domain_value.size() << " header_bytes=" << cookie_value.size() << " adopted=" << (nuah_roblox_cookie_value().empty() ? "no" : "yes")
              << " forwarded=" << (preserve_primed_session ? "no" : "yes") << '\n';
  }
}
static inline unsigned long parse_ulong_env(const char* name, unsigned long def, unsigned long min_val = 0, unsigned long max_val = ~0UL, bool* is_explicit = nullptr) {
  const char* raw = ::getenv(name);
  if (!raw || !*raw) return def;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(raw, &end, 10);
  if (end != raw && *end == '\0' && parsed >= min_val && parsed <= max_val) {
    if (is_explicit) *is_explicit = true;
    return parsed;
  }
  return def;
}
static inline bool parse_bool_env(const char* name, bool def = false, bool* is_explicit = nullptr) {
  const char* raw = ::getenv(name);
  if (!raw || !*raw) return def;
  if (is_explicit) *is_explicit = true;
  return std::strcmp(raw, "0") != 0;
}
template <typename Setter>
static inline bool set_jni_field_impl(JNIEnv* env, const char* name, const char* sig, jclass klass, Setter&& setter) {
  const jfieldID field = env->GetFieldID(klass, name, sig);
  if (!field) return fail_false(env, name);
  setter(field);
  return !env->ExceptionCheck();
}
[[maybe_unused]] static inline bool set_jni_field(JNIEnv* env, jobject o, jclass k, const char* n, const char* v) {
  return set_jni_field_impl(env, n, "Ljava/lang/String;", k, [&](jfieldID f) { jstring t = env->NewStringUTF(v ? v : ""); env->SetObjectField(o, f, t); env->DeleteLocalRef(t); });
}
[[maybe_unused]] static inline bool set_jni_field(JNIEnv* env, jobject o, jclass k, const char* n, bool v) { return set_jni_field_impl(env, n, "Z", k, [&](jfieldID f) { env->SetBooleanField(o, f, v ? JNI_TRUE : JNI_FALSE); }); }
[[maybe_unused]] static inline bool set_jni_field(JNIEnv* env, jobject o, jclass k, const char* n, jint v) { return set_jni_field_impl(env, n, "I", k, [&](jfieldID f) { env->SetIntField(o, f, v); }); }
[[maybe_unused]] static inline bool set_jni_field(JNIEnv* env, jobject o, jclass k, const char* n, jlong v) { return set_jni_field_impl(env, n, "J", k, [&](jfieldID f) { env->SetLongField(o, f, v); }); }
[[maybe_unused]] static inline bool set_jni_field(JNIEnv* env, jobject o, jclass k, const char* n, jfloat v) { return set_jni_field_impl(env, n, "F", k, [&](jfieldID f) { env->SetFloatField(o, f, v); }); }
static inline bool populate_platform_params(JNIEnv* env, jobject platform, jclass platform_class, const char* content) {
  return set_jni_field(env, platform, platform_class, "assetFolderPath", content ? content : "") && set_jni_field(env, platform, platform_class, "dpiScale", 1.0f) && set_jni_field(env, platform, platform_class, "isKeyboardDevice", true) && set_jni_field(env, platform, platform_class, "isMouseDevice", true) && set_jni_field(env, platform, platform_class, "isTouchDevice", true) && set_jni_field(env, platform, platform_class, "viewportHeightMm", 190) && set_jni_field(env, platform, platform_class, "viewportWidthMm", 340);
}
// cpu64Bit must stay a bool literal: JNI_TRUE would select the jint overload (sig "I") and fail init.
static inline bool set_device_identity_fields(JNIEnv* env, jobject o, jclass k) {
  return set_jni_field(env, o, k, "appBuildVariant", "release") && set_jni_field(env, o, k, "appVersion", "Roblox") && set_jni_field(env, o, k, "cpu64Bit", true) && set_jni_field(env, o, k, "deviceName", "Nuah Linux PC") && set_jni_field(env, o, k, "deviceSku", "x86_64") && set_jni_field(env, o, k, "manufacturer", "Nuah") && set_jni_field(env, o, k, "osVersion", "36") && set_jni_field(env, o, k, "socModel", "x86_64");
}
bool populate_device_params(JNIEnv* env, jobject device, jclass device_class) {
  return set_device_identity_fields(env, device, device_class) && set_jni_field(env, device, device_class, "country", "US") && set_jni_field(env, device, device_class, "deviceTotalMemoryMB", 4096) && set_jni_field(env, device, device_class, "displayPhysicalHeightPixels", 720) && set_jni_field(env, device, device_class, "displayPhysicalWidthPixels", 1280) && set_jni_field(env, device, device_class, "displayResolution", "1280x720") && set_jni_field(env, device, device_class, "isChrome", false) && set_jni_field(env, device, device_class, "isLowRamDevice", false) && set_jni_field(env, device, device_class, "largeMemoryClass", 512) && set_jni_field(env, device, device_class, "lowMemoryKillerBackgroundAppThreshold", 0L) && set_jni_field(env, device, device_class, "lowMemoryKillerForegroundAppThreshold", 0L) && set_jni_field(env, device, device_class, "memoryClass", 256) && set_jni_field(env, device, device_class, "networkType", "WIFI") && set_jni_field(env, device, device_class, "testDeviceName", "");
}
struct AutoValueBuilder {
  JNIEnv* env;
  jclass builder_class;
  jobject builder;
  std::string ret_sig;
  AutoValueBuilder(JNIEnv* e, jclass bc, jobject b, const char* ret_class) : env(e), builder_class(bc), builder(b), ret_sig(std::string(")") + ret_class) {}
  template <typename Fn>
  bool call_helper(const char* name, const char* param_sig, Fn&& fn) {
    if (!builder) return false;
    const jmethodID m = env->GetMethodID(builder_class, name, (std::string("(") + param_sig + ret_sig).c_str());
    if (!m) return fail_false(env, name);
    builder = fn(m);
    if (!builder || env->ExceptionCheck()) return fail_false(env, name);
    return true;
  }
  bool call_object(const char* name, const char* param_sig, jobject val) { return call_helper(name, param_sig, [&](jmethodID m) { return env->CallObjectMethod(builder, m, val); }); }
  bool call_string(const char* name, const char* val, bool optional = false) {
    if (!builder) return false;
    jstring text = env->NewStringUTF(val ? val : "");
    if (!text) return false;
    const jmethodID m = env->GetMethodID(builder_class, name, (std::string("(Ljava/lang/String;") + ret_sig).c_str());
    if (!m) { clear_java_exception(env, name); env->DeleteLocalRef(text); return optional; }
    builder = env->CallObjectMethod(builder, m, text);
    env->DeleteLocalRef(text);
    if (!builder || env->ExceptionCheck()) { clear_java_exception(env, name); return false; }
    return true;
  }
  bool call_long(const char* name, jlong val) { return call_helper(name, "J", [&](jmethodID m) { return env->CallObjectMethod(builder, m, val); }); }
  bool call_int(const char* name, jint val) { return call_helper(name, "I", [&](jmethodID m) { return env->CallObjectMethod(builder, m, val); }); }
  bool call_bool(const char* name, jboolean val) { return call_helper(name, "Z", [&](jmethodID m) { return env->CallObjectMethod(builder, m, val); }); }
  jobject build(const char* out_class) {
    if (!builder) return nullptr;
    const jmethodID m = env->GetMethodID(builder_class, "build", (std::string("()") + out_class).c_str());
    if (!m) return fail_null(env, "build");
    jobject res = env->CallObjectMethod(builder, m);
    if (!res || env->ExceptionCheck()) return fail_null(env, "build");
    return res;
  }
};
// InitParams is an AutoValue: build via the app builder, never AllocObject (aborts in GetObjectClass).
jobject make_real_init_params(JNIEnv* env, jobject activity) {
  if (!env) return nullptr;
  jclass init_class = env->FindClass("com/roblox/engine/jni/autovalue/InitParams"), builder_class = env->FindClass("com/roblox/engine/jni/autovalue/InitParams$Builder"), platform_class = env->FindClass("com/roblox/engine/jni/model/PlatformParams"), device_class = env->FindClass("com/roblox/engine/jni/model/DeviceParams");
  if (!init_class || !builder_class || !platform_class || !device_class) return fail_null(env, "InitParams classes");
  const jmethodID builder_method = env->GetStaticMethodID(init_class, "builder", "()Lcom/roblox/engine/jni/autovalue/InitParams$Builder;"), platform_ctor = env->GetMethodID(platform_class, "<init>", "()V"), device_ctor = env->GetMethodID(device_class, "<init>", "()V");
  if (!builder_method || !platform_ctor || !device_ctor) return fail_null(env, "InitParams constructors");
  jobject builder = env->CallStaticObjectMethod(init_class, builder_method), platform = env->NewObject(platform_class, platform_ctor), device = env->NewObject(device_class, device_ctor);
  if (!builder || !platform || !device || env->ExceptionCheck()) return fail_null(env, "InitParams builder allocation");
  const char* content = nonempty_env("NUAH_CONTENT_PATH");
  if (!populate_platform_params(env, platform, platform_class, content) || !populate_device_params(env, device, device_class) || env->ExceptionCheck()) return fail_null(env, "InitParams field population");
  AutoValueBuilder b(env, builder_class, builder, "Lcom/roblox/engine/jni/autovalue/InitParams$Builder;");
  if (!b.call_object("setPlatformParams", "Lcom/roblox/engine/jni/model/PlatformParams;", platform) || !b.call_object("setDeviceParams", "Lcom/roblox/engine/jni/model/DeviceParams;", device) || !b.call_string("setBaseURL", "https://www.roblox.com/") || !b.call_string("setUserAgent", "Roblox/Android Nuah") || !b.call_bool("setIsPotato", JNI_FALSE) ||
      !b.call_bool("setIsTablet", JNI_FALSE) || !b.call_bool("setIsVrDevice", JNI_FALSE) || !b.call_string("setBuildVariant", "release") || !b.call_object("setVrContext", "Landroid/app/Activity;", activity)) {
    return nullptr;
  }
  return b.build("Lcom/roblox/engine/jni/autovalue/InitParams;");
}
// StartGameParams is an AutoValue: same builder rule as InitParams.
jobject make_real_start_game_params(JNIEnv* env, jobject surface, jobject activity, const LaunchRequest& request, const char* content_path) {
  if (!env || !surface || !activity) return nullptr;
  jclass params_class = env->FindClass("com/roblox/engine/jni/autovalue/StartGameParams"), builder_class = env->FindClass("com/roblox/engine/jni/autovalue/StartGameParams$Builder"), platform_class = env->FindClass("com/roblox/engine/jni/model/PlatformParams"), device_class = env->FindClass("com/roblox/engine/jni/model/DeviceParams");
  if (!params_class || !builder_class || !platform_class || !device_class) return fail_null(env, "StartGameParams classes");
  bool use_platform_subclass = parse_bool_env("NUAH_START_PLATFORM_SUBCLASS", false, nullptr);
  jclass game_platform_class = nullptr; jmethodID game_platform_ctor = nullptr;
  if (use_platform_subclass) {
    game_platform_class = env->FindClass("ml/a");
    if (game_platform_class) { game_platform_ctor = env->GetMethodID(game_platform_class, "<init>", "(Lcom/roblox/engine/jni/model/PlatformParams;)V"); }
    if (!game_platform_class || !game_platform_ctor) {
      clear_java_exception(env, "StartGameParams platform subclass");
      use_platform_subclass = false;
      game_platform_class = nullptr;
      game_platform_ctor = nullptr;
    }
  }
  const jmethodID builder_method = env->GetStaticMethodID(params_class, "builder", "()Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;"), platform_ctor = env->GetMethodID(platform_class, "<init>", "()V");
  if (!builder_method || !platform_ctor) return fail_null(env, "StartGameParams constructors");
  jobject builder = env->CallStaticObjectMethod(params_class, builder_method), base_platform = env->NewObject(platform_class, platform_ctor);
  jobject platform = base_platform;
  if (use_platform_subclass && base_platform) {
    platform = env->NewObject(game_platform_class, game_platform_ctor, base_platform);
    if (platform) {
      const jfieldID tablet = env->GetFieldID(game_platform_class, "isTablet", "Z");
      if (tablet) env->SetBooleanField(platform, tablet, JNI_FALSE);
      if (env->ExceptionCheck()) return fail_null(env, "StartGameParams platform tablet flag");
    }
  }
  if (!builder || !platform || env->ExceptionCheck()) return fail_null(env, "StartGameParams allocation");
  const char* content = content_path ? content_path : "";
  if (!populate_platform_params(env, platform, platform_class, content)) return fail_null(env, "StartGameParams platform fields");
  const char* const empty = "";
  const std::string access_code = request.game_instance_id.value_or(""), reserved_server_access_code = request.reserved_server_access_code.value_or(""), launch_data = request.launch_data.value_or(""), call_id = request.call_id.value_or("");
  const jint join_request_type = nuah_roblox_user_id() > 0 && access_code.empty() ? 1 : 2;
  const jlong place_id = static_cast<jlong>(std::stoll(request.place_id));
  const char* username = nonempty_env("NUAH_ROBLOX_USERNAME");
  const jobject vr_context = ::getenv("NUAH_ENABLE_VR") ? activity : nullptr;
  AutoValueBuilder b(env, builder_class, builder, "Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;");
  const bool fields_ok = b.call_object("setSurface", "Landroid/view/Surface;", surface) && b.call_object("setPlatformParams", "Lcom/roblox/engine/jni/model/PlatformParams;", platform) && b.call_object("setDeviceParams", "Lcom/roblox/engine/jni/model/DeviceParams;", nullptr) && b.call_long("setConversationId", 0) && b.call_long("setReferredByPlayerId", 0) && b.call_string("setAccessCode", access_code.c_str()) && b.call_string("setCallId", call_id.c_str()) && b.call_string("setEventId", empty) && b.call_string("setGameId", empty) &&
                         b.call_string("setGameIdToExclude", empty, /*optional=*/true) && b.call_string("setGameJoinContext", empty) && b.call_bool("setIsUnder13", JNI_FALSE) && b.call_string("setIsoContext", empty) && b.call_string("setJoinAttemptId", "00000000-0000-0000-0000-000000000000") && b.call_string("setJoinAttemptOrigin", empty) &&
                         b.call_int("setJoinRequestType", join_request_type) && b.call_string("setLaunchData", launch_data.c_str()) && b.call_string("setLinkCode", empty) && b.call_long("setPlaceId", place_id) && b.call_string("setReferralPage", "WebView") && b.call_string("setReservedServerAccessCode", reserved_server_access_code.c_str()) &&
                         b.call_long("setUserId", nuah_roblox_user_id()) && b.call_string("setUsername", username ? username : empty) && b.call_object("setVrContext", "Landroid/app/Activity;", vr_context);
  if (!fields_ok) return nullptr;
  return b.build("Lcom/roblox/engine/jni/autovalue/StartGameParams;");
}
jobject make_real_platform_params(JNIEnv* env, const char* content_path) {
  if (!env) return nullptr;
  jclass klass = env->FindClass("com/roblox/engine/jni/model/PlatformParams");
  const jmethodID ctor = klass ? env->GetMethodID(klass, "<init>", "()V") : nullptr;
  if (!klass || !ctor) return fail_null(env, "PlatformParams constructor");
  jobject params = env->NewObject(klass, ctor);
  if (!params) return fail_null(env, "PlatformParams allocation");
  if (!populate_platform_params(env, params, klass, content_path)) return fail_null(env, "PlatformParams fields");
  return params;
}
jobject make_real_start_app_params(JNIEnv* env, jobject surface, jobject platform_params) {
  if (!env || !surface || !platform_params) return nullptr;
  jclass params_class = env->FindClass("com/roblox/engine/jni/autovalue/StartAppParams"), builder_class = env->FindClass("com/roblox/engine/jni/autovalue/StartAppParams$Builder");
  if (!params_class || !builder_class) return fail_null(env, "StartAppParams classes");
  const jmethodID builder_method = env->GetStaticMethodID(params_class, "builder", "()Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;");
  if (!builder_method) return fail_null(env, "StartAppParams.builder");
  jobject builder = env->CallStaticObjectMethod(params_class, builder_method);
  if (!builder || env->ExceptionCheck()) return fail_null(env, "StartAppParams.builder call");
  AutoValueBuilder b(env, builder_class, builder, "Lcom/roblox/engine/jni/autovalue/StartAppParams$Builder;");
  const bool fields_ok = b.call_object("setSurface", "Landroid/view/Surface;", surface) && b.call_object("setPlatformParams", "Lcom/roblox/engine/jni/model/PlatformParams;", platform_params) && b.call_string("setAppStarterPlace", "") && b.call_string("setAppStarterScript", "") && b.call_long("setAppUserId", nuah_roblox_user_id()) && b.call_bool("setIsUnder13", JNI_FALSE) &&
                         b.call_string("setUsername", "") && b.call_int("setMembershipType", 0) && b.call_string("setSelectedTheme", "Dark");
  if (!fields_ok) return nullptr;
  return b.build("Lcom/roblox/engine/jni/autovalue/StartAppParams;");
}
bool install_device_static_params(JNIEnv* env) {
  if (!env) return false;
  jclass bridge = env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface"), params_class = env->FindClass("com/roblox/engine/jni/model/DeviceStaticParams");
  if (!bridge || !params_class) return fail_false(env, "DeviceStaticParams classes");
  const jmethodID ctor = env->GetMethodID(params_class, "<init>", "()V"), setter = env->GetStaticMethodID(bridge, "setDeviceStaticParams", "(Lcom/roblox/engine/jni/model/DeviceStaticParams;)V");
  if (!ctor || !setter) return fail_false(env, "DeviceStaticParams methods");
  jobject params = env->NewObject(params_class, ctor);
  if (!params) return fail_false(env, "DeviceStaticParams allocation");
  if (!set_device_identity_fields(env, params, params_class) || env->ExceptionCheck()) return fail_false(env, "DeviceStaticParams fields");
  env->CallStaticVoidMethod(bridge, setter, params);
  if (env->ExceptionCheck()) return fail_false(env, "DeviceStaticParams setter");
  return true;
}
std::vector<std::filesystem::path> image_candidates(const NativeLaunchOptions& options) {
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
    }
  }
  throw std::runtime_error("selected APK set has no x86_64 lib/x86_64/libroblox.so");
}
std::filesystem::path runtime_directory() {
  std::array<char, 4096> path{};
  const auto size = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size <= 0 || static_cast<std::size_t>(size) >= path.size() - 1) { throw std::runtime_error("cannot locate Nuah runtime directory"); }
  return std::filesystem::path(std::string(path.data(), size)).parent_path();
}
class RuntimeDataLock {
 public:
  explicit RuntimeDataLock(const std::filesystem::path& data_directory) {
    std::error_code error;
    std::filesystem::create_directories(data_directory, error);
    if (error) { throw std::runtime_error("cannot create Nuah data directory for runtime lock: " + error.message()); }
    const auto lock_path = data_directory / "nuah-runtime.lock";
    fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) { throw std::runtime_error("cannot open Nuah runtime lock: " + std::string(std::strerror(errno))); }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const int saved_errno = errno;
      ::close(fd_);
      fd_ = -1;
      if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
        throw std::runtime_error(
            "another Nuah runtime already owns this data directory; "
            "close it before launching a second room");
      }
      throw std::runtime_error("cannot acquire Nuah runtime lock: " + std::string(std::strerror(saved_errno)));
    }
    const std::string owner = std::to_string(static_cast<long long>(::getpid())) + "\n";
    if (::ftruncate(fd_, 0) != 0) { throw std::runtime_error("cannot reset Nuah runtime lock"); }
    if (::write(fd_, owner.data(), owner.size()) != static_cast<ssize_t>(owner.size())) { throw std::runtime_error("cannot write Nuah runtime lock"); }
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
void checkpoint_asset_cache(const std::filesystem::path& runtime_data_directory, const std::filesystem::path& apk) {
  if (!parse_bool_env("NUAH_CACHE_CHECKPOINT", true, nullptr)) return;
  const auto app_data = runtime_data_directory / (apk.filename().string() + "_");
  const auto database = app_data / "files/appData/rbx-storage.db";
  const auto wal = std::filesystem::path(database.string() + "-wal");
  std::error_code error;
  if (!std::filesystem::is_regular_file(database, error) || error || !std::filesystem::is_regular_file(wal, error) || error || std::filesystem::file_size(wal, error) == 0 || error) { return; }
  using Sqlite3 = void;
  using Open = int (*)(const char*, Sqlite3**, int, const char*);
  using Exec = int (*)(Sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
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
  const int open_result = open(database.c_str(), &connection, kOpenReadWrite | kOpenFullMutex, nullptr);
  if (open_result == 0 && connection) {
    const int result = exec(connection, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    if (bootstrap_trace()) { std::cerr << "nuah cache: WAL checkpoint result=" << result << " path=" << database << '\n'; }
  } else if (bootstrap_trace()) {
    std::cerr << "nuah cache: WAL checkpoint skipped (sqlite open result=" << open_result << ") path=" << database << '\n';
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
    const auto written = ::write(fd, member.bytes.data() + offset, member.bytes.size() - offset);
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
  if (!bootstrap_trace() || !env || !params) return;
  jclass klass = env->GetObjectClass(params);
  if (!klass) return;
  const jmethodID access = env->GetMethodID(klass, "accessCode", "()Ljava/lang/String;"), user = env->GetMethodID(klass, "userId", "()J"), type = env->GetMethodID(klass, "joinRequestType", "()I"), place = env->GetMethodID(klass, "placeId", "()J");
  jstring access_value = access ? static_cast<jstring>(env->CallObjectMethod(params, access)) : nullptr;
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
               access_size, static_cast<long long>(user_value), static_cast<int>(type_value), static_cast<long long>(place_value));
  if (access_value) env->DeleteLocalRef(access_value);
  env->DeleteLocalRef(klass);
}
int run_nuah_jni(const NativeLaunchOptions& options, const std::filesystem::path& apk) {
  const bool fast_mvp = fast_mvp_enabled(), init_only = parse_bool_env("NUAH_INIT_ONLY", false, nullptr);
  std::string asset_apks;
  for (const auto& candidate : image_candidates(options)) {
    if (!std::filesystem::is_regular_file(candidate)) continue;
    if (!asset_apks.empty()) asset_apks += ':';
    asset_apks += std::filesystem::absolute(candidate).string();
  }
  if (asset_apks.empty() || ::setenv("NUAH_APK_PATHS", asset_apks.c_str(), 1) != 0) { throw std::runtime_error("cannot configure Android asset APK paths"); }
  const auto app_data_directory = std::filesystem::absolute(options.data_directory.value_or(std::filesystem::temp_directory_path() / "nuah-data")) / (options.apk.filename().string() + "_");
  std::error_code app_data_error;
  std::filesystem::create_directories(app_data_directory, app_data_error);
  if (app_data_error || ::setenv("ANDROID_APP_DATA_DIR", app_data_directory.c_str(), 1) != 0) { throw std::runtime_error("cannot configure Android app-private data directory"); }
  configure_mesa_shader_cache(app_data_directory);
  configure_mesa_submit_thread();
  (void)prepare_app_native_libraries(options);
  const auto app_library_directory = app_data_directory / "lib";
  std::string bionic_library_path;
  const auto nuah_runtime_directory = runtime_directory();
  if (std::filesystem::is_regular_file(nuah_runtime_directory / "libnuah_host_bridge.so")) bionic_library_path += nuah_runtime_directory.string() + ":";
  bionic_library_path += app_library_directory.string() + ":" + app_data_directory.string() + "**";
  std::vector<std::filesystem::path> host_android_dirs;
  if (const char* configured = nonempty_env("NUAH_ART_LIBRARY")) { host_android_dirs.emplace_back(std::filesystem::path(configured).parent_path()); }
  host_android_dirs.emplace_back("/usr/local/lib64/art");
  for (const auto& directory : host_android_dirs) {
    if (!std::filesystem::is_regular_file(directory / "libandroidfw.so")) continue;
    bionic_library_path += ":" + directory.string(); break;
  }
  if (::setenv("BIONIC_LD_LIBRARY_PATH", bionic_library_path.c_str(), 1) != 0) { throw std::runtime_error("cannot configure Android app native-library path"); }
  const bool disable_early_sync = !parse_bool_env("NUAH_ANDROID_SYNC_EARLY", true, nullptr);
  if (!disable_early_sync && !::getenv("NUAH_ANDROID_SYNC")) {
    if (::setenv("NUAH_ANDROID_SYNC", "1", 1) != 0) { throw std::runtime_error("cannot enable early Android synchronization"); }
  }
  report_bootstrap_stage("ANDROID_DLOPEN_CONSTRUCTORS");
  auto image = load_apk_library(apk, "lib/x86_64/libroblox.so");
  report_bootstrap_stage("PRE_JNI_BIND");
  std::unique_ptr<NuahNativeSession, decltype(&nuah_native_session_destroy)> session(nullptr, nuah_native_session_destroy);
  session.reset(nuah_native_session_create());
  if (!session) throw std::runtime_error("cannot create Nuah native session");
  if (!std::getenv("NUAH_DISABLE_PROPERTY_PATCH")) (void)patch_loaded_module_property_import(image);
  NuahJvm* jvm = nuah_native_session_jvm(session.get());
#define RBX_S(n, s) {"com/roblox/engine/jni/NativeSettingsInterface", n, s, "Java_com_roblox_engine_jni_NativeSettingsInterface_" n}
#define RBX_I(n, s) \
  { "com/roblox/engine/jni/NativeInputInterface", n, s, "Java_com_roblox_engine_jni_NativeInputInterface_" n }
  static constexpr struct {
    const char *klass, *name, *sig, *sym;
  } kRobloxNatives[] = {
      RBX_S("nativeSetBaseUrl", "(Ljava/lang/String;Ljava/lang/String;)V"), RBX_S("nativeSetRobloxChannel", "(Ljava/lang/String;)V"),
      RBX_S("nativeSetExceptionReasonFilename", "(Ljava/lang/String;)V"), RBX_S("nativeOverrideChannelPlatformName", "(Ljava/lang/String;)V"),
      RBX_S("nativeOverrideChannelPlatformName2", "(Ljava/lang/String;)V"), RBX_S("nativeInitFastLog", "()V"),
      RBX_S("nativeSetRobloxVersion", "(Ljava/lang/String;)V"), RBX_S("nativeSetUserId", "(Ljava/lang/String;)V"),
      RBX_S("nativeSetCacheDirectory", "(Ljava/lang/String;)V"), RBX_S("nativeSetFilesDirectory", "(Ljava/lang/String;)V"),
      RBX_S("nativeInitCrashpad", "(Lcom/roblox/engine/jni/model/NativeInitCrashpadParams;)Z"), RBX_S("nativeInitAppCrashpadReporter", "(Lcom/roblox/engine/jni/model/NativeInitCrashpadParams;)Z"),
      RBX_S("nativeRunCrashpadHandler", "([Ljava/lang/String;)I"),
      {"com/roblox/engine/jni/NativeGLInterface", "nativePassKeyEvent", "(ZIIZ)V", "Java_com_roblox_engine_jni_NativeGLInterface_nativePassKeyEvent"},
      RBX_I("nativePassMouseMove", "(FFFF)V"), RBX_I("nativePassMouseButton", "(FFZI)V"),
      RBX_I("nativePassMouseWheel", "(FFF)V"), RBX_I("nativeGetMainWindowIsMouseLockedCenter", "()Z"),
      {"com/roblox/client/startup/MainGameActivity", "nativeSetAssetPath", "(Ljava/lang/String;)V", "Java_com_roblox_client_startup_MainGameActivity_nativeSetAssetPath"},
  };
#undef RBX_S
#undef RBX_I
  for (const auto& b : kRobloxNatives) {
    if (void* fn = image.symbol(b.sym)) { (void)nuah_jvm_bind_native(jvm, b.klass, b.name, b.sig, fn); }
  }
  g_roblox_set_multiple_cookies = reinterpret_cast<RobloxSetMultipleCookies>(image.symbol("Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetMultipleCookies"));
  g_roblox_get_cookies_for_domain = reinterpret_cast<RobloxGetCookiesForDomain>(image.symbol("Java_com_roblox_engine_jni_NativeSettingsInterface_nativeGetCookiesForDomain"));
  const struct {
    const char* name;
    const char* sig;
    void* fn;
  } kCookies[] = {
      {"nativeGetCookiesInNetscapeFormat", "(Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void*>(&nuah_native_get_cookies_netscape)},
      {"nativeGetCookiesForDomain", "(Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void*>(&nuah_native_get_cookies_for_domain)},
      {"nativeSetMultipleCookies", "(Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(&nuah_native_set_cookies)},
  };
  for (const auto& c : kCookies) {
    if (!nuah_jvm_bind_native(jvm, "com/roblox/engine/jni/NativeSettingsInterface", c.name, c.sig, c.fn)) { throw std::runtime_error("Roblox cookie boundary registration failed"); }
  }
  void* running_architecture = image.symbol("Java_com_roblox_engine_jni_NativeSettingsInterface_getRunningArchitecture");
  if (!running_architecture) running_architecture = reinterpret_cast<void*>(&nuah_native_get_running_architecture);
  if (!nuah_jvm_bind_native(jvm, "com/roblox/engine/jni/NativeSettingsInterface", "getRunningArchitecture", "()I", running_architecture)) {
    throw std::runtime_error("NativeSettingsInterface.getRunningArchitecture registration failed");
  }
  report_bootstrap_stage("APPLICATION_STATE_SETUP");
  if (!nuah_jvm_dispatch_application_create(jvm)) { throw std::runtime_error("Roblox application state setup failed"); }
  auto* bootstrap_env = reinterpret_cast<JNIEnv*>(nuah_jvm_jni_env(jvm));
  if (bootstrap_trace()) std::fprintf(stderr, "nuah native: DeviceStaticParams begin\n");
  if (!install_device_static_params(bootstrap_env)) { throw std::runtime_error("Roblox DeviceStaticParams contract could not be initialized"); }
  if (bootstrap_trace()) std::fprintf(stderr, "nuah native: DeviceStaticParams done\n");
  if (bootstrap_trace()) std::fprintf(stderr, "nuah native: cookie prime begin\n");
  prime_roblox_cookie_store(bootstrap_env, true);
  if (bootstrap_trace()) std::fprintf(stderr, "nuah native: cookie prime done\n");
  constexpr const char* kGameActivity = "com/google/androidgamesdk/GameActivity";
  constexpr const char* kInitializeSignature =
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
      "Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J";
  struct NativeRequirement {
    const char* member;
    const char* signature;
  };
  void* exported_initialize = image.symbol("Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
  if (!exported_initialize || !nuah_jvm_bind_native(jvm, kGameActivity, "initializeNativeCode", kInitializeSignature, exported_initialize)) {
    throw std::runtime_error("libroblox.so exposes no bindable GameActivity initializeNativeCode");
  }
  std::unique_ptr<NuahWindowSession, decltype(&nuah_window_session_destroy)> window(nuah_window_session_create(options.width, options.height, "Roblox"), nuah_window_session_destroy);
  if (!window) throw std::runtime_error("cannot create Nuah SDL/Vulkan window");
  void* surface = nuah_native_session_surface(session.get(), nuah_window_session_native_window(window.get()));
  if (!surface) throw std::runtime_error("cannot create Nuah Android Surface façade");
  report_bootstrap_stage("ACTIVITY_ON_CREATE");
  if (::getenv("NUAH_PAUSE_BEFORE_ACTIVITY_CREATE")) {
    std::cerr << "nuah bootstrap: paused before MainGameActivity.onCreate (pid=" << ::getpid() << ")\n";
    (void)::raise(SIGSTOP);
  }
  if (!nuah_jvm_dispatch_activity_create(jvm)) { throw std::runtime_error("MainGameActivity.onCreate failed"); }
  surface = nuah_native_session_surface(session.get(), nuah_window_session_native_window(window.get()));
  if (!surface) throw std::runtime_error("cannot obtain GameActivity holder Surface");
  const auto data_directory = options.data_directory.value_or(std::filesystem::temp_directory_path() / "nuah-data");
  std::error_code data_error;
  std::filesystem::create_directories(data_directory, data_error);
  if (data_error) { throw std::runtime_error("cannot create Nuah game data directory: " + data_error.message()); }
  report_bootstrap_stage("GAMEACTIVITY_INITIALIZE");
  if (nuah_jvm_capture_native_handle(jvm)) {
    if (bootstrap_trace()) { std::cerr << "nuah native: reusing GameActivity.onCreate native handle\n"; }
  } else if (!nuah_native_session_initialize_game(session.get(), "com.roblox.client", data_directory.c_str())) {
    throw std::runtime_error("GameActivity initializeNativeCode returned no native handle");
  }
  std::filesystem::path content_directory, extra_content_directory;
  if (const char* override_path = nonempty_env("NUAH_CONTENT_PATH")) {
    content_directory = override_path;
    extra_content_directory = content_directory.parent_path() / "ExtraContent";
  } else {
    const auto app_assets = app_data_directory / "files/assets", app_assets_legacy = app_data_directory / "assets", data_assets = data_directory / "assets";
    const std::array<std::filesystem::path, 3> asset_roots = {app_assets, app_assets_legacy, data_assets};
    for (const auto& root : asset_roots) {
      const auto candidate = root / "content", candidate_extra = root / "ExtraContent";
      if (!asset_tree_has_files(candidate)) continue;
      content_directory = candidate; extra_content_directory = candidate_extra;
      break;
    }
    if (content_directory.empty()) {
      content_directory = app_assets / "content"; extra_content_directory = app_assets / "ExtraContent";
    }
  }
  std::filesystem::create_directories(content_directory, data_error);
  if (data_error) { throw std::runtime_error("cannot create Roblox content directory: " + data_error.message()); }
  auto content_path = std::filesystem::absolute(content_directory).string();
  if (!content_path.ends_with('/')) content_path += '/';
  if (::setenv("NUAH_CONTENT_PATH", content_path.c_str(), 1) != 0) { throw std::runtime_error("cannot publish Roblox content path"); }
  nuah_roblox_java_facade_set_content_path(content_path.c_str());
  if (bootstrap_trace()) {
    std::cerr << "nuah assets: content=" << content_path << " files=" << (asset_tree_has_files(content_directory) ? 1 : 0) << " extra=" << std::filesystem::absolute(extra_content_directory)
              << " extra_files=" << (asset_tree_has_files(extra_content_directory) ? 1 : 0) << '\n';
  }
  prefetch_local_asset_pages({content_directory, extra_content_directory});
  auto* env = reinterpret_cast<JNIEnv*>(nuah_jvm_jni_env(jvm));
  jclass main_activity = env->FindClass("com/roblox/client/startup/MainGameActivity"), native_gl = env->FindClass("com/roblox/engine/jni/NativeGLInterface");
  jobject init_params = make_real_init_params(env, reinterpret_cast<jobject>(nuah_jvm_game_activity(jvm)));
  using SetAssetPath = void (*)(JNIEnv*, jclass, jstring);
  using SetInitParams = void (*)(JNIEnv*, jclass, jobject);
  using InitClientSettings = jint (*)(JNIEnv*, jclass, jstring, jstring, jstring);
  using PostClientSettings = void (*)(JNIEnv*, jclass, jobject);
  using InitWithParams = void (*)(JNIEnv*, jclass, jobject);
  const SetAssetPath set_asset_path = reinterpret_cast<SetAssetPath>(image.symbol("Java_com_roblox_client_startup_MainGameActivity_nativeSetAssetPath"));
  const SetInitParams set_init_params = reinterpret_cast<SetInitParams>(image.symbol("Java_com_roblox_client_startup_MainGameActivity_nativeAppBridgeSetInitParams"));
  const InitClientSettings init_client_settings = reinterpret_cast<InitClientSettings>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeInitClientSettings"));
  const PostClientSettings post_client_settings = reinterpret_cast<PostClientSettings>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativePostClientSettingsLoadedInitialization3"));
  const InitWithParams init_with_params = reinterpret_cast<InitWithParams>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2InitWithParams"));
  if (!main_activity || !init_params || !set_asset_path || (!native_gl || !init_with_params) || (!fast_mvp && !set_init_params) || !init_client_settings || !post_client_settings) {
    throw std::runtime_error("Roblox MainGameActivity pre-start JNI contract is unavailable");
  }
  report_bootstrap_stage("ROBLOX_ASSET_PATH");
  set_asset_path(env, main_activity, env->NewStringUTF(content_path.c_str()));
  if (!::getenv("NUAH_DISABLE_CRASH_HANDLER")) { nuah_bootstrap_diagnostics_install_signal_handler(); }
  if (::getenv("NUAH_PAUSE_BEFORE_INIT")) {
    std::cerr << "nuah bootstrap: paused before Roblox parameter handoff (pid=" << ::getpid() << ")\n";
    (void)::raise(SIGSTOP);
  }
  if (!::getenv("NUAH_ANDROID_SYNC") && ::setenv("NUAH_ANDROID_SYNC", "1", 1) != 0) { throw std::runtime_error("cannot select Android synchronization adapter"); }
  report_bootstrap_stage("ROBLOX_CLIENT_SETTINGS_INIT");
  const char* settings_json = nonempty_env("NUAH_CLIENT_SETTINGS_JSON");
  std::string settings_storage, default_settings_storage;
  const bool disable_msaa = parse_bool_env("NUAH_DISABLE_MSAA", false, nullptr);
  if (const char* settings_path = nonempty_env("NUAH_CLIENT_SETTINGS_PATH")) {
    std::ifstream settings_file(settings_path, std::ios::binary);
    if (!settings_file) { throw std::runtime_error("cannot open client-settings response: " + std::string(settings_path)); }
    std::ostringstream settings_stream;
    settings_stream << settings_file.rdbuf();
    settings_storage = settings_stream.str();
    if (settings_storage.empty()) { throw std::runtime_error("client-settings response is empty"); }
    settings_json = settings_storage.c_str();
  }
  if (!settings_json || !*settings_json) {
    const char* requested_backend = nonempty_env("NUAH_GRAPHICS_BACKEND");
    const bool prefer_opengl = requested_backend && (std::strcmp(requested_backend, "opengl") == 0 || std::strcmp(requested_backend, "gles") == 0 || std::strcmp(requested_backend, "gl") == 0);
    const unsigned int logical_cpus = std::thread::hardware_concurrency();
    const char* performance_mode = nonempty_env("NUAH_PERFORMANCE_MODE");
    const bool quality_mode = performance_mode && (std::strcmp(performance_mode, "quality") == 0 || std::strcmp(performance_mode, "full") == 0);
    const bool requested_low_mode = performance_mode && (std::strcmp(performance_mode, "low") == 0 || std::strcmp(performance_mode, "balanced") == 0);
    const bool turbo_mode = (!performance_mode || !*performance_mode || std::strcmp(performance_mode, "turbo") == 0 || std::strcmp(performance_mode, "fast") == 0);
    const bool low_end_profile = !quality_mode && (turbo_mode || requested_low_mode || (logical_cpus > 0 && logical_cpus <= 4));
    const char* governor = engine_governor_profile();
    const bool governor_balanced = governor && std::strcmp(governor, "balanced") == 0;
    unsigned long scheduler_threads = governor_balanced   ? 2UL
                                      : low_end_profile   ? (logical_cpus <= 2 ? 1UL : 2UL)
                                      : logical_cpus <= 1 ? 1UL
                                                          : std::min<unsigned long>(4, std::max<unsigned int>(2, logical_cpus - 1));
    if (const unsigned long parsed = parse_ulong_env("NUAH_TASK_THREADS", scheduler_threads, 1, 64); parsed != 2) scheduler_threads = parsed;
    unsigned long target_fps = parse_ulong_env("NUAH_TARGET_FPS", 60, 30, 240);
    unsigned long dynamic_render_kilo_pixels = parse_ulong_env("NUAH_DYNAMIC_RENDER_KILOPIXELS", 0, 64, 4096);
    unsigned long render_texture_budget_ms = parse_ulong_env("NUAH_RENDER_TEXTURE_BUDGET_MS", governor_balanced ? 2UL : (low_end_profile ? 4UL : 0UL), 1, 32);
    unsigned long asset_provider_threads = parse_ulong_env("NUAH_ASSET_PROVIDER_THREADS", low_end_profile ? 1UL : 0UL, 1, 4);
    unsigned long asset_workflow_sleep_us = parse_ulong_env("NUAH_ASSET_WORKFLOW_SLEEP_US", low_end_profile ? 0UL : 150000UL, 0, 1000000UL);
    const char* disk_cache = ::getenv("NUAH_ASSET_DISK_CACHE");
    const bool enable_disk_asset_cache = parse_bool_env("NUAH_ASSET_DISK_CACHE");
    const bool disable_slow_rendering = parse_bool_env("NUAH_FAST_RENDER", turbo_mode);
    const bool prioritize_render_over_assets = parse_bool_env("NUAH_ASSET_BACKGROUND", turbo_mode);
    const bool allow_async_transcode = parse_bool_env("NUAH_ASSET_TRANSCODE_ASYNC", low_end_profile);
    const bool disable_texture_pack_generator = parse_bool_env("NUAH_DISABLE_TEXTURE_PACK_GENERATOR");
    unsigned long frm_quality = parse_ulong_env("NUAH_FRM_QUALITY", turbo_mode ? 1UL : 0UL, 0, 21);
    const bool no_background_http_retry = parse_bool_env("NUAH_HTTP_BACKGROUND_NO_RETRY", true);
    unsigned long http_request_timeout_ms = parse_ulong_env("NUAH_HTTP_REQUEST_TIMEOUT_MS", 0, 100, 60000);
    const bool disable_profile_configuration = parse_bool_env("NUAH_DISABLE_PROFILE_CONFIGURATION", turbo_mode);
    static constexpr const char* kDesktopHttpRetryOverrides =
        "503:POST:gamejoin.roblox.com:/v1/{join-endpoint}:linear:10:6|"
        "429:POST:gamejoin.roblox.com:/v1/{join-endpoint}:linear:10:6|"
        "429:GET:users.roblox.com:/v1/users:none:0:0";
    std::ostringstream generated_settings;
    generated_settings << "{\"applicationSettings\":{"
                       << "\"FFlagDebugGraphicsPreferOpenGL\":" << (prefer_opengl ? "true" : "false") << ',' << "\"FFlagDebugGraphicsPreferVulkan\":" << (prefer_opengl ? "false" : "true") << ','
                       << "\"FFlagDebugGraphicsDisableVulkan\":" << (prefer_opengl ? "true" : "false") << ',' << "\"FIntTaskSchedulerAutoThreadLimit\":\"" << scheduler_threads << "\","
                       << "\"DFIntTaskSchedulerJobInGameThreads\":\"" << scheduler_threads << "\","
                       << "\"FIntTaskSchedulerAsyncTasksMinimumThreadCount\":\"1\","
                       << "\"FIntTaskSchedulerThreadMin\":\"0\","
                       << "\"DFIntTaskSchedulerTargetFps\":\"" << target_fps << "\","
                       << "\"DFFlagDebugDisableRbxTransportDummyClient\":true}}";
    default_settings_storage = generated_settings.str();
    settings_storage = packaged_client_settings(app_data_directory);
    std::string& target_settings = !settings_storage.empty() ? settings_storage : default_settings_storage;
    const std::string graphics_opengl = prefer_opengl ? "true" : "false";
    const std::string graphics_vulkan = prefer_opengl ? "false" : "true";
    const std::string scheduler_count = "\"" + std::to_string(scheduler_threads) + "\"";
    const std::string fps_count = "\"" + std::to_string(target_fps) + "\"";
    const std::string sleep_count = "\"" + std::to_string(asset_workflow_sleep_us) + "\"";
    const std::string frm_count = "\"" + std::to_string(frm_quality) + "\"";
    const std::string worker_count = "\"" + std::to_string(asset_provider_threads) + "\"";
    const std::string kilo_pixels = "\"" + std::to_string(dynamic_render_kilo_pixels) + "\"";
    const std::string texture_budget = "\"" + std::to_string(render_texture_budget_ms) + "\"";
    const std::string timeout = "\"" + std::to_string(http_request_timeout_ms) + "\"";
    struct Setting { std::string_view key; std::string value; bool apply; };
    for (const auto& s : {
             Setting{"FFlagDebugGraphicsPreferOpenGL", graphics_opengl, true}, Setting{"FFlagDebugGraphicsPreferVulkan", graphics_vulkan, true},
             Setting{"FFlagDebugGraphicsDisableVulkan", graphics_opengl, true}, Setting{"FStringGraphicsTextureManager2DenyPattern2", "\".*\"", !prefer_opengl},
             Setting{"FStringGraphicsVulkanShaderMTDenyPattern", "\"4318:.*\"", !prefer_opengl}, Setting{"FIntTaskSchedulerAutoThreadLimit", scheduler_count, true},
             Setting{"DFIntTaskSchedulerJobInGameThreads", scheduler_count, true}, Setting{"FIntTaskSchedulerAsyncTasksMinimumThreadCount", "\"1\"", true},
             Setting{"FIntTaskSchedulerThreadMin", "\"0\"", true}, Setting{"DFIntTaskSchedulerTargetFps", fps_count, true},
             Setting{"DFIntDebugDynamicRenderKiloPixels", kilo_pixels, dynamic_render_kilo_pixels != 0},
             Setting{"FIntRenderTextureProcessingBudgetMilliseconds", texture_budget, render_texture_budget_ms != 0},
             Setting{"DFIntAssetProviderAssetCacheReadThreadCount", worker_count, asset_provider_threads != 0},
             Setting{"DFIntAssetProviderCallbackExecutorThreadCount", worker_count, asset_provider_threads != 0},
             Setting{"DFIntAssetProviderWorkflowExecutorSleepMicroSeconds", sleep_count, true},
             Setting{"DFFlagAlwaysSkipDiskCache", enable_disk_asset_cache ? "false" : "true", disk_cache && *disk_cache},
             Setting{"FFlagSlowDownRendering", "false", disable_slow_rendering},
             Setting{"FFlagAssetProviderDisablePrioAwareStdWorkerThreadTaskFactory", "false", prioritize_render_over_assets},
             Setting{"FFlagAssetProviderDisablePriorityAwareDeferredWritesTaskFactory", "false", prioritize_render_over_assets},
             Setting{"FFlagSimRuntimeContentTranscodeBlockingCall", "false", allow_async_transcode},
             Setting{"FFlagEnableTexturePackGeneratorOnClient2", "false", disable_texture_pack_generator},
             Setting{"FFlagTexturePackGeneratorUseRaw", "false", disable_texture_pack_generator},
             Setting{"DFFlagDisableRccTexturePackGenerator", "true", disable_texture_pack_generator},
             Setting{"DebugForceMSAASamples", "\"0\"", disable_msaa || turbo_mode}, Setting{"DebugFRMOptionalMSAALevelOverride", "\"0\"", disable_msaa || turbo_mode},
             Setting{"DFIntDebugFRMQualityLevelOverride", frm_count, frm_quality != 0},
             Setting{"DFIntHttpServiceRequestTimeoutMs", timeout, http_request_timeout_ms != 0}, Setting{"DFIntHttpResponseDefaultTimeoutMillis", timeout, http_request_timeout_ms != 0},
             Setting{"FFlagPlayersGetProfileConfigurationEnabled", "false", disable_profile_configuration},
             Setting{"FFlagPlayersGetProfileConfigurationFromUserIdEnabled", "false", disable_profile_configuration},
             Setting{"DFFlagPopulateUserInformationForPlayers", "false", disable_profile_configuration},
             Setting{"DFFlagDebugDisableRbxTransportDummyClient", "true", true},
         }) {
      if (s.apply) set_client_setting(target_settings, s.key, s.value);
    }
    if (turbo_mode) {
      static constexpr std::pair<std::string_view, std::string_view> kTurboFlags[] = {
          {"FFlagDisablePostFx", "true"}, {"FFlagDebugDisableBloom", "true"}, {"FFlagDebugDisableColorCorrection", "true"}, {"FFlagDebugDisableSunRays", "true"},
          {"FFlagDebugDisableDepthOfField", "true"}, {"FFlagGlobalWind", "false"}, {"FFlagEnableGlobalWind", "false"}, {"FFlagFastGPULightGrid", "true"},
          {"FFlagRenderLightGridDirectLight", "false"}, {"FIntFRMMinGrassDistance", "\"0\""}, {"FIntFRMMaxGrassDistance", "\"0\""}, {"FIntRenderShadowIntensity", "\"0\""},
          {"FIntShadowMapBias", "\"0\""}, {"FIntRenderShadowMapResolution", "\"0\""}, {"FFlagDebugDisableClouds", "true"}, {"FFlagRenderClouds", "false"},
          {"FFlagRenderEnableVolumetricClouds", "false"}, {"FFlagDebugSkyDisableAtmosphere", "true"}, {"FFlagRenderEnableLowEndLOD", "true"}, {"FFlagEnableTextureStreaming2", "true"},
          {"FFlagRenderEnableTextureStreaming", "true"}, {"FFlagRenderTextureManagerFlushUnused", "true"}, {"FIntTexturePoolSizeMB", "\"256\""}, {"FFlagRenderMeshLodStreaming", "true"},
          {"FFlagRenderMeshPartBatching", "true"}, {"FFlagRenderEnableInstancing", "true"}, {"FFlagRenderEnableTextureAtlas", "true"}, {"FFlagDebugDisableSkyReflections", "true"},
          {"FFlagDebugDisableAtmosphereReflections", "true"}, {"FFlagRenderNoColorCorrection", "true"}, {"FFlagDebugDisableToneMapping", "true"}, {"FFlagRenderGrassOrnament", "false"},
          {"FFlagRenderEnableGrass", "false"}, {"FFlagRenderEnableDecalStreaming", "true"}, {"FFlagRenderBatchParticles", "true"}, {"FFlagRenderOptimizeLightGrid", "true"},
          {"FFlagRenderDisableShadowMapResolution", "true"}, {"FFlagRenderDisableVolumetricAtmosphere", "true"}, {"FIntTerrainArraySliceSize", "\"4\""}, {"FFlagLuauNativeCodeGen", "true"},
          {"FFlagLuauNativeCodeGenMode2", "true"}, {"FIntLuauNativeCodeGenThreshold", "\"0\""}, {"FFlagLuauFastVariables", "true"}, {"FFlagLuauFastVariableAccess", "true"},
          {"FFlagLuauTablePrealloc", "true"}, {"FIntLuauGcStepMultiplier", "\"100\""}, {"FIntLuauGcGoalRatio", "\"200\""}, {"FFlagLuauIncrementalGC", "true"},
          {"FFlagLuauVector3Bytecode", "true"}, {"DFIntTaskSchedulerSleepToleranceMs", "\"0\""}, {"FFlagTaskSchedulerAvoidSleeping", "true"}, {"FIntPhysicsMaxSubSteps", "\"1\""},
          {"FFlagFastPhysicsStep", "true"}, {"FFlagAdaptivePhysicsStepping", "true"}, {"FFlagSimdLinearAlgebra", "true"}, {"FIntRenderJobWaitBudgetMicroseconds", "\"0\""},
          {"FFlagDebugDisableShadowCasting", "true"}, {"FFlagRenderFixParticlesCulling", "true"}, {"FFlagPhysicsStepCycleFix", "true"}, {"FFlagTaskSchedulerLimitTargetFpsTo240", "false"},
          {"FFlagGameBasicSettingsFramerateCap5", "false"},
      };
      for (const auto& [key, value] : kTurboFlags) set_client_setting(target_settings, key, value);
    }
    if (no_background_http_retry) {
      const std::string dsv2 = std::string("\"") + kDesktopHttpRetryOverrides + "\"";
      for (const char* key :
           {"DFStringHttpRetryOverridesDsv2", "FStringHttpRetryOverridesDsv2Static", "DFStringHttpRetryOverridesDsv2_PlaceFilter", "FStringHttpRetryOverridesDsv2Static_PlaceFilter"}) {
        set_client_setting(target_settings, key, dsv2);
      }
      set_client_setting(target_settings, "DFIntHttpRbxApiMaxRetryCount", "\"0\"");
    }
    settings_json = target_settings.c_str();
    if (bootstrap_trace()) {
      std::cerr << "nuah graphics: using applicationSettings "
                   "(host overrides applied) backend="
                << (prefer_opengl ? "opengl" : "vulkan") << '\n';
    }
  }
  if (settings_json && *settings_json) {
    if (settings_storage.empty()) settings_storage = settings_json;
    set_client_setting(settings_storage, "DFFlagDebugDisableRbxTransportDummyClient", "true");
    settings_json = settings_storage.c_str();
  }
  try {
    const std::array<std::filesystem::path, 2> setting_paths = {app_data_directory / "ClientSettings/ClientAppSettings.json", app_data_directory / "files/ClientSettings/ClientAppSettings.json"};
    for (const auto& path : setting_paths) {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream out(path, std::ios::trunc | std::ios::binary);
      if (out) out << settings_json;
    }
  } catch (...) {}
  const jstring settings = env->NewStringUTF(settings_json);
  const jstring settings_signature = env->NewStringUTF("");
  const jstring settings_application = env->NewStringUTF("GoogleAndroidApp");
  const jint settings_status = init_client_settings(env, native_gl, settings, settings_signature, settings_application);
  if (bootstrap_trace()) { std::cerr << "nuah native: nativeInitClientSettings status=" << settings_status << '\n'; }
  if (settings_status != 0) { throw std::runtime_error("Roblox client-settings initialization failed with status " + std::to_string(settings_status)); }
  report_bootstrap_stage("ROBLOX_CLIENT_SETTINGS_POST_INIT");
  const jclass history_class = env->FindClass("java/util/ArrayList");
  const jobject history = history_class ? env->AllocObject(history_class) : nullptr;
  if (!history) throw std::runtime_error("cannot create empty client-settings history list");
  const bool post_client_settings_enabled = [] { if (parse_bool_env("NUAH_CLIENT_SETTINGS_POST", false)) return true; if (parse_bool_env("NUAH_SKIP_CLIENT_SETTINGS_POST", false)) return false; return false; }();
  if (post_client_settings_enabled) {
    post_client_settings(env, native_gl, history);
  } else if (bootstrap_trace()) {
    std::cerr << "nuah native: skipping client-settings post-init (set "
                 "NUAH_CLIENT_SETTINGS_POST=1 to opt in)\n";
  }
  using GlobalInit = void (*)(JNIEnv*, jclass);
  using UpdateAdapterInit = void (*)(JNIEnv*, jclass);
  const auto global_init = reinterpret_cast<GlobalInit>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeGameGlobalInit"));
  const auto update_adapter_init = reinterpret_cast<UpdateAdapterInit>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeUpdateAdapterInit"));
  report_bootstrap_stage("ROBLOX_GAME_GLOBAL_INIT");
  if (global_init) global_init(env, native_gl);
  if (update_adapter_init) update_adapter_init(env, native_gl);
  if (!fast_mvp) {
    report_bootstrap_stage("ROBLOX_INIT_PARAMS_DIAGNOSTIC");
    if (!parse_bool_env("NUAH_SKIP_ROBLOX_INIT_PARAMS", false)) {
      set_init_params(env, main_activity, init_params);
    } else if (bootstrap_trace()) {
      std::cerr << "nuah native: skipping MainGameActivity parameter setter "
                   "for ABI comparison\n";
    }
  }
  report_bootstrap_stage(fast_mvp ? "ROBLOX_FAST_V2_INIT" : "ROBLOX_GAME_V2_INIT");
  if (bootstrap_trace()) { std::cerr << "nuah native: NativeGLInterface V2 init\n"; }
  init_with_params(env, native_gl, init_params);
  report_bootstrap_stage("ROBLOX_V2_INIT_COMPLETE");
  if (init_only) {
    std::cerr << "nuah native: V2 initialization completed (init-only)\n";
    return 0;
  }
  constexpr NativeRequirement kMvpRequirements[] = {
      {"onStartNative", "(J)V"}, {"onResumeNative", "(J)V"}, {"onSurfaceCreatedNative", "(JLandroid/view/Surface;)V"}, {"onSurfaceChangedNative", "(JLandroid/view/Surface;III)V"},
      {"onTouchEventNative", "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z"}, {"onKeyDownNative", "(JLandroid/view/KeyEvent;)Z"}, {"onKeyUpNative", "(JLandroid/view/KeyEvent;)Z"},
  };
  for (const auto& requirement : kMvpRequirements) {
    if (!nuah_jvm_find_registered_native(jvm, kGameActivity, requirement.member, requirement.signature)) {
      throw std::runtime_error(
          "GameActivity initializeNativeCode did not register required "
          "callback " +
          std::string(requirement.member) + " " + requirement.signature);
    }
  }
  report_bootstrap_stage("GAMEACTIVITY_REGISTRATION_COMPLETE");
  report_bootstrap_stage("GAMEACTIVITY_START");
  if (!nuah_native_session_dispatch_lifecycle(session.get(), "onStartNative")) { throw std::runtime_error("GameActivity start callback is unavailable"); }
  report_bootstrap_stage("GAMEACTIVITY_RESUME");
  if (!nuah_native_session_dispatch_lifecycle(session.get(), "onResumeNative")) { throw std::runtime_error("GameActivity resume callback is unavailable"); }
  if (!std::getenv("NUAH_DISABLE_DELAYED_FOCUS")) {
    const unsigned long focus_delay_ms = parse_ulong_env("NUAH_PRESTART_FOCUS_DELAY_MS", 0);
    if (focus_delay_ms != 0) ::usleep(focus_delay_ms * 1000U);
    report_bootstrap_stage("GAMEACTIVITY_FOCUS");
    const bool focus_ok = nuah_native_session_dispatch_window_focus(session.get(), 1) != 0;
    if (bootstrap_trace()) { std::cerr << "nuah native: activity focus-before-surface status=" << (focus_ok ? 1 : 0) << '\n'; }
  }
  const auto* native_window = nuah_window_session_native_window(window.get());
  const int width = nuah_native_window_width(native_window);
  const int height = nuah_native_window_height(native_window);
  if (!nuah_native_session_dispatch_surface_created(session.get(), surface) || !nuah_native_session_dispatch_surface_changed(session.get(), surface, 0, width, height)) {
    throw std::runtime_error("GameActivity surface lifecycle callback is unavailable");
  }
  report_bootstrap_stage("GRAPHICS_LIFECYCLE_ACTIVE");
  nuah_roblox_java_facade_set_launch_surface(reinterpret_cast<jobject>(surface));
  const bool async_game_start = parse_bool_env("NUAH_ASYNC_GAME_START", false, nullptr);
  if (options.uri && !options.uri->empty()) {
    const auto request = parse_roblox_uri(*options.uri);
    nuah_roblox_java_facade_set_launch_place_id(static_cast<jlong>(std::stoll(request.place_id)));
    const std::string request_access_code = request.game_instance_id.value_or("");
    const std::string request_reserved_server_access_code = request.reserved_server_access_code.value_or("");
    const jint request_join_type = nuah_roblox_user_id() > 0 && request_access_code.empty() ? 1 : 2;
    nuah_roblox_java_facade_set_start_game_params(request_access_code.c_str(), request_reserved_server_access_code.c_str(), nuah_roblox_user_id(), request_join_type);
    const auto start_game = reinterpret_cast<jint (*)(JNIEnv*, jclass, jobject)>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartGameWithParam"));
    using StartApp = void (*)(JNIEnv*, jclass, jobject);
    const auto start_app = reinterpret_cast<StartApp>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartAppWithParams"));
    using StartLuaApp = void (*)(JNIEnv*, jclass);
    const auto start_lua_app = reinterpret_cast<StartLuaApp>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeStartLuaAppDM"));
    using UpdateSurfaceGame = void (*)(JNIEnv*, jclass, jobject, jobject, jobject);
    const auto update_surface_game = reinterpret_cast<UpdateSurfaceGame>(image.symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams"));
    const auto start_params = make_real_start_game_params(env, reinterpret_cast<jobject>(surface), reinterpret_cast<jobject>(nuah_jvm_game_activity(jvm)), request, content_path.c_str());
    trace_start_game_params(env, start_params);
    const auto activity = reinterpret_cast<jobject>(nuah_jvm_game_activity(jvm));
    const auto platform_params = make_real_platform_params(env, content_path.c_str());
    const bool start_app_with_params = parse_bool_env("NUAH_START_APP_WITH_PARAMS", false, nullptr);
    const auto start_app_params = start_app_with_params ? make_real_start_app_params(env, reinterpret_cast<jobject>(surface), platform_params) : nullptr;
    if (!native_gl || !start_game || !start_params || !update_surface_game || !activity || !platform_params || (start_app_with_params && (!start_app || !start_app_params))) {
      throw std::runtime_error("Roblox direct game-start JNI contract is unavailable");
    }
    nuah_window_session_show_loading(window.get());
    const bool start_lua_app_dm = parse_bool_env("NUAH_START_LUA_APP_DM", false, nullptr);
    if (start_lua_app_dm && start_lua_app) {
      report_bootstrap_stage("ROBLOX_APP_BRIDGE_START_LUA");
      start_lua_app(env, native_gl);
      clear_java_exception(env, "nativeAppBridgeStartLuaAppDM");
      if (bootstrap_trace()) { std::cerr << "nuah native: nativeAppBridgeStartLuaAppDM invoked\n"; }
    }
    const bool surface_view_callback_before_start = parse_bool_env("NUAH_SURFACE_VIEW_CALLBACK_BEFORE_START", false, nullptr);
    if (surface_view_callback_before_start) {
      report_bootstrap_stage("ROBLOX_GAME_SURFACE_VIEW_CALLBACK_PRESTART");
      const int queued = nuah_jvm_dispatch_surface_view_lifecycle(jvm, nuah_native_window_width(native_window), nuah_native_window_height(native_window));
      if (bootstrap_trace()) { std::cerr << "nuah native: prestart SurfaceView callback queued=" << queued << '\n'; }
    }
    if (start_app_with_params) {
      report_bootstrap_stage("ROBLOX_APP_START_WITH_PARAMS");
      start_app(env, native_gl, start_app_params);
      clear_java_exception(env, "nativeAppBridgeV2StartAppWithParams");
      if (bootstrap_trace()) { std::cerr << "nuah native: nativeAppBridgeV2StartAppWithParams invoked\n"; }
    }
    report_bootstrap_stage("ROBLOX_GAME_START");
    prime_roblox_cookie_store(env, false);
    jint start_result = 0;
    if (!async_game_start) {
      start_result = start_game(env, native_gl, start_params);
    } else {
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
        if (!async_state->vm || async_state->vm->AttachCurrentThread(&worker_env, nullptr) != JNI_OK || !worker_env) {
          async_state->done.store(true, std::memory_order_release);
          return;
        }
        async_state->result.store(async_state->start(worker_env, reinterpret_cast<jclass>(async_state->native_gl), async_state->start_params), std::memory_order_release);
        worker_env->DeleteGlobalRef(async_state->native_gl);
        worker_env->DeleteGlobalRef(async_state->start_params);
        async_state->refs_deleted.store(true, std::memory_order_release);
        async_state->done.store(true, std::memory_order_release);
        async_state->vm->DetachCurrentThread();
      });
      const unsigned long focus_delay_ms = parse_ulong_env("NUAH_ASYNC_FOCUS_DELAY_MS", 500);
      if (focus_delay_ms != 0) ::usleep(focus_delay_ms * 1000U);
      if (!std::getenv("NUAH_DISABLE_DELAYED_FOCUS")) {
        report_bootstrap_stage("GAMEACTIVITY_FOCUS");
        const bool focus_ok = nuah_native_session_dispatch_window_focus(session.get(), 1) != 0;
        if (bootstrap_trace()) { std::cerr << "nuah native: async window focus hasFocus=true status=" << (focus_ok ? 1 : 0) << '\n'; }
      }
      const unsigned long start_timeout_ms = parse_ulong_env("NUAH_ASYNC_START_TIMEOUT_MS", 15000);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(start_timeout_ms);
      while (!async_state->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) { ::usleep(1000); }
      if (!async_state->done.load(std::memory_order_acquire)) {
        std::cerr << "nuah native: async Roblox start timed out after " << start_timeout_ms << " ms\n";
        starter.detach();
        start_result = -ETIMEDOUT;
      } else {
        starter.join();
        if (!async_state->refs_deleted.exchange(true, std::memory_order_acq_rel)) {
          env->DeleteGlobalRef(async_state->native_gl);
          env->DeleteGlobalRef(async_state->start_params);
        }
        start_result = async_state->result.load(std::memory_order_acquire);
      }
    }
    const unsigned long settle_ms = parse_ulong_env("NUAH_GAME_START_SETTLE_MS", 500);
    if (settle_ms != 0) ::usleep(settle_ms * 1000U);
    const bool explicit_surface_update = parse_bool_env("NUAH_EXPLICIT_SURFACE_UPDATE", false, nullptr);
    const bool callback_surface_update = parse_bool_env("NUAH_SURFACE_CALLBACK_UPDATE", false, nullptr);
    const bool surface_view_callback = parse_bool_env("NUAH_SURFACE_VIEW_CALLBACK", false, nullptr);
    if (surface_view_callback) {
      report_bootstrap_stage("ROBLOX_GAME_SURFACE_VIEW_CALLBACK");
      const int queued = nuah_jvm_dispatch_surface_view_lifecycle(jvm, nuah_native_window_width(native_window), nuah_native_window_height(native_window));
      if (bootstrap_trace()) { std::cerr << "nuah native: SurfaceView callback queued=" << queued << '\n'; }
    }
    if (parse_bool_env("NUAH_FOCUS_AFTER_START", false, nullptr)) {
      report_bootstrap_stage("GAMEACTIVITY_FOCUS_AFTER_START");
      const bool focus_ok = nuah_native_session_dispatch_window_focus(session.get(), 1) != 0;
      if (bootstrap_trace()) { std::cerr << "nuah native: post-start focus status=" << (focus_ok ? 1 : 0) << '\n'; }
    }
    if (callback_surface_update) {
      report_bootstrap_stage("ROBLOX_GAME_SURFACE_CALLBACK");
      if (!nuah_native_session_dispatch_surface_changed(session.get(), surface, 0, nuah_native_window_width(native_window), nuah_native_window_height(native_window))) {
        throw std::runtime_error("GameActivity surface callback update was unavailable");
      }
      if (parse_bool_env("NUAH_FOCUS_AFTER_SURFACE", false, nullptr)) {
        report_bootstrap_stage("GAMEACTIVITY_FOCUS");
        (void)nuah_native_session_dispatch_window_focus(session.get(), 1);
      }
    } else if (explicit_surface_update) {
      report_bootstrap_stage("ROBLOX_GAME_SURFACE_UPDATE");
      update_surface_game(env, native_gl, reinterpret_cast<jobject>(surface), platform_params, activity);
      if (parse_bool_env("NUAH_FOCUS_AFTER_SURFACE", false, nullptr)) {
        report_bootstrap_stage("GAMEACTIVITY_FOCUS");
        (void)nuah_native_session_dispatch_window_focus(session.get(), 1);
      }
    } else if (bootstrap_trace()) {
      std::cerr << "nuah native: Roblox-owned UGC surface transition\n";
    }
    if (bootstrap_trace()) { std::cerr << "nuah native: nativeAppBridgeV2StartGameWithParam placeId=" << request.place_id << " status=" << start_result << '\n'; }
  }
  nuah_input_bind_native_session(session.get());
  const unsigned int input_sleep_us = input_poll_sleep_us();
  if (bootstrap_trace()) { std::cerr << "nuah input: host poll interval=" << input_sleep_us << "us\n"; }
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
struct NativeIsolationThreadArgs {
  const NativeLaunchOptions* options;
  const std::filesystem::path* apk;
  int result;
  std::string error;
};
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
int run_nuah_jni_isolated_impl(const NativeLaunchOptions& options, const std::filesystem::path& apk) {
  void* mapping = ::mmap(nullptr, sizeof(NuahBootstrapDiagnostics), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) { throw std::runtime_error("cannot create native bootstrap diagnostics"); }
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
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != parent_pid) _exit(70);
#ifdef PR_SET_TIMERSLACK
    (void)::prctl(PR_SET_TIMERSLACK, 50000UL, 0, 0, 0);
#endif
#endif
    if (!::getenv("NUAH_DISABLE_CRASH_HANDLER")) { nuah_bootstrap_diagnostics_install_signal_handler(); }
    constexpr std::size_t kNativeBootstrapStack = 256u * 1024u * 1024u;
    pthread_attr_t attributes{};
    if (::pthread_attr_init(&attributes) != 0 || ::pthread_attr_setstacksize(&attributes, kNativeBootstrapStack) != 0) {
      (void)::pthread_attr_destroy(&attributes);
      std::cerr << "nuah bootstrap: cannot allocate native bootstrap stack\n";
      _exit(70);
    }
    NativeIsolationThreadArgs arguments{&options, &apk, 1, {}};
    pthread_t worker{};
    const int create_result = ::pthread_create(&worker, &attributes, run_nuah_jni_isolated_on_large_stack, &arguments);
    (void)::pthread_attr_destroy(&attributes);
    if (create_result != 0) {
      std::cerr << "nuah bootstrap: cannot start native bootstrap thread (errno=" << create_result << ")\n";
      _exit(70);
    }
    (void)::pthread_join(worker, nullptr);
    if (!arguments.error.empty()) {
      std::cerr << "nuah bootstrap: native initialization failed before JNI_OnLoad: " << arguments.error << '\n';
      _exit(70);
    }
    _exit(arguments.result == 0 ? 0 : 70);
  }
  struct sigaction forward_action{};
  struct sigaction previous_term{};
  struct sigaction previous_int{};
  forward_action.sa_handler = forward_isolated_child_signal;
  sigemptyset(&forward_action.sa_mask);
  forward_action.sa_flags = 0;
  g_isolated_child_pid = child;
  const bool term_handler_installed = ::sigaction(SIGTERM, &forward_action, &previous_term) == 0;
  const bool int_handler_installed = ::sigaction(SIGINT, &forward_action, &previous_int) == 0;
  int status = 0;
  pid_t waited = -1;
  do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
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
    if (crash_module.parent_path() == "/tmp" && crash_module.filename().string().starts_with("nuah-module-")) {
      std::error_code ignored;
      std::filesystem::remove(crash_module, ignored);
    }
  }
  const std::string stage = result.stage[0] ? result.stage : "NO_STAGE";
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  if (WIFSIGNALED(status)) {
    std::ostringstream message;
    message << "native bootstrap terminated by signal " << WTERMSIG(status) << " at " << stage;
    if (result.abort_seen) {
      message << "; crash caller " << (result.module_path[0] ? result.module_path : "(unmapped)") << "+0x" << std::hex << result.module_offset << std::dec << " (pc=0x" << std::hex << result.caller
              << std::dec << " tid=" << result.thread_id << ")";
      if (result.parent_module_offset) { message << "; parent module offset +0x" << std::hex << result.parent_module_offset << std::dec; }
    }
    if (result.fault_address) { message << "; fault-address=0x" << std::hex << result.fault_address << std::dec; }
    if (result.abort_message[0]) { message << "; Android abort message: " << result.abort_message; }
    if (result.last_log[0]) { message << "; last Android log: " << result.last_log; }
    if (result.last_property[0]) { message << "; last Android property: " << result.last_property; }
    throw std::runtime_error(message.str());
  }
  throw std::runtime_error("native bootstrap exited with status " + std::to_string(WEXITSTATUS(status)) + " at " + stage);
}
int run_nuah_jni_isolated(const NativeLaunchOptions& options, const std::filesystem::path& apk) {
  return run_nuah_jni_isolated_impl(options, apk);
}
int run_bionic_loader(const std::filesystem::path& image) {
  const auto root = runtime_directory();
  const auto linker = root / "bionic/lib64/linker64";
  const auto helper = root / "bionic/nuah-bionic-loader";
  const auto library_path = (root / "bionic/lib64").string();
  if (!std::filesystem::is_regular_file(linker) || !std::filesystem::is_regular_file(helper)) { throw std::runtime_error("Nuah bionic runtime bundle is missing linker64 or helper"); }
  const auto child = ::fork();
  if (child < 0) throw std::runtime_error("cannot start bionic loader helper");
  if (child == 0) {
    ::setenv("LD_LIBRARY_PATH", library_path.c_str(), 1);
    ::execl(linker.c_str(), linker.c_str(), helper.c_str(), image.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child) { throw std::runtime_error("cannot wait for bionic loader helper"); }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  if (WIFEXITED(status)) { throw std::runtime_error("bionic loader helper exited with status " + std::to_string(WEXITSTATUS(status))); }
  throw std::runtime_error("bionic loader helper terminated by signal " + std::to_string(WTERMSIG(status)));
}
}  // namespace
int run_native(const NativeLaunchOptions& options) {
  NativeLaunchOptions launch = options;
  const char* performance_mode = std::getenv("NUAH_PERFORMANCE_MODE");
  const bool quality_mode = performance_mode && (std::strcmp(performance_mode, "quality") == 0 || std::strcmp(performance_mode, "full") == 0);
  const bool turbo_mode = (!performance_mode || !*performance_mode || std::strcmp(performance_mode, "turbo") == 0 || std::strcmp(performance_mode, "fast") == 0);
  if (!quality_mode && !launch.dimensions_explicit && launch.width == 1280 && launch.height == 720) {
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
  if (!std::filesystem::is_regular_file(launch.apk)) { throw std::runtime_error("native APK does not exist: " + launch.apk.string()); }
  if (launch.width <= 0 || launch.height <= 0) { throw std::runtime_error("native window dimensions must be positive"); }
  const auto runtime_data_directory = std::filesystem::absolute(launch.data_directory.value_or(std::filesystem::temp_directory_path() / "nuah-data"));
  if (!launch.data_directory) launch.data_directory = runtime_data_directory;
  RuntimeDataLock runtime_data_lock(runtime_data_directory);
  checkpoint_asset_cache(runtime_data_directory, launch.apk);
  const auto profile_tmp = runtime_data_directory / "tmp";
  const char* requested_tmp = std::getenv("NUAH_TMPDIR");
  const std::filesystem::path temporary_directory = requested_tmp && *requested_tmp ? std::filesystem::path(requested_tmp) : profile_tmp;
  std::error_code temporary_error;
  std::filesystem::create_directories(temporary_directory, temporary_error);
  if (temporary_error || ::setenv("TMPDIR", temporary_directory.c_str(), 1) != 0) { throw std::runtime_error("cannot configure Nuah profile temporary directory"); }
  if (bootstrap_trace()) { std::cerr << "nuah runtime: temporary directory=" << temporary_directory << '\n'; }
  const char* surface_lock = ::getenv("NUAH_LOCK_SURFACE_SIZE");
  if (!surface_lock || std::strcmp(surface_lock, "0") != 0) {
    const std::string width = std::to_string(launch.width);
    const std::string height = std::to_string(launch.height);
    if (::setenv("NUAH_LOCK_SURFACE_SIZE", "1", 1) != 0 || ::setenv("NUAH_SURFACE_WIDTH", width.c_str(), 1) != 0 || ::setenv("NUAH_SURFACE_HEIGHT", height.c_str(), 1) != 0) {
      throw std::runtime_error("cannot configure native Android surface size");
    }
  }
  if (!std::getenv("NUAH_DISABLE_SESSION_DISCOVERY")) {
    (void)discover_session_cookie();
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
    std::cerr << "nuah native: API-36 bionic loader accepted libroblox.so from " << image_apk << '\n';
    return 0;
  }
  try {
    const int result = run_nuah_jni_isolated(launch, image_apk);
    checkpoint_asset_cache(runtime_data_directory, launch.apk);
    return result;
  } catch (...) {
    checkpoint_asset_cache(runtime_data_directory, launch.apk);
    throw;
  }
}
}  // namespace nuah
