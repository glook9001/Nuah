#include "nuah/atl_backend.hpp"
#include "nuah/apk_loader.hpp"
#include "nuah/bootstrap_diagnostics.h"
#include "nuah/input_bridge.h"
#include "nuah/launch_uri.hpp"
#include "nuah/native_session.h"
#include "nuah/nuah_jvm.h"
#include "nuah/window_session.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/stat.h>
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
}

void prime_roblox_cookie_store(JNIEnv* env) {
  if (const char* skip = ::getenv("NUAH_SKIP_COOKIE_PRIME"); skip && *skip) {
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
      std::fprintf(stderr, "nuah cookie: skipping early native-store prime\n");
    return;
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
  if (const char* home = ::getenv("HOME"); home && *home) {
    const std::filesystem::path home_path(home);
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
      const std::string_view user_marker = "rbxuid=";
      const std::size_t user_pos = line.find(user_marker);
      if (user_pos != std::string::npos) {
        const std::size_t user_begin = user_pos + user_marker.size();
        const std::size_t user_end =
            line.find_first_not_of("0123456789", user_begin);
        const std::string user_id = line.substr(
            user_begin, user_end == std::string::npos
                           ? std::string::npos
                           : user_end - user_begin);
        if (!user_id.empty() && user_id.size() <= 20) {
          (void)::setenv("NUAH_ROBLOX_USER_ID", user_id.c_str(), 1);
        }
      }
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
      set_bool_field(platform, platform_class, "isTouchDevice", JNI_FALSE) &&
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
  jclass game_platform_class = env->FindClass("ml/a");
  jclass device_class =
      env->FindClass("com/roblox/engine/jni/model/DeviceParams");
  if (!params_class || !builder_class || !platform_class ||
      !game_platform_class || !device_class) {
    clear_java_exception(env, "StartGameParams classes");
    return nullptr;
  }
  const jmethodID builder_method = env->GetStaticMethodID(
      params_class, "builder",
      "()Lcom/roblox/engine/jni/autovalue/StartGameParams$Builder;");
  const jmethodID platform_ctor = env->GetMethodID(platform_class, "<init>", "()V");
  const jmethodID game_platform_ctor = env->GetMethodID(
      game_platform_class, "<init>",
      "(Lcom/roblox/engine/jni/model/PlatformParams;)V");
  if (!builder_method || !platform_ctor || !game_platform_ctor) {
    clear_java_exception(env, "StartGameParams constructors");
    return nullptr;
  }
  jobject builder = env->CallStaticObjectMethod(params_class, builder_method);
  jobject base_platform = env->NewObject(platform_class, platform_ctor);
  jobject platform = base_platform
                         ? env->NewObject(game_platform_class,
                                         game_platform_ctor, base_platform)
                         : nullptr;
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
  const jfieldID tablet = env->GetFieldID(game_platform_class, "isTablet", "Z");
  if (!tablet) {
    clear_java_exception(env, "StartGameParams platform tablet flag");
    return nullptr;
  }
  env->SetBooleanField(platform, tablet, JNI_FALSE);
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
      call_string("setUsername", empty) &&
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
      set_bool("isTouchDevice", JNI_FALSE) &&
      set_int("viewportHeightMm", 190) &&
      set_int("viewportWidthMm", 340);
  if (!ok || env->ExceptionCheck()) {
    clear_java_exception(env, "PlatformParams fields");
    return nullptr;
  }
  return params;
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
  // Reuse ATL's APK-native extraction routine. This is the Android contract
  // System.loadLibrary expects; do not fabricate host substitutes for app
  // libraries such as libzstd-jni.
  (void)prepare_atl_native_libraries(options);
  const auto app_library_directory = app_data_directory / "lib";
  // The installed ATL linker owns the process's one libc/TLS domain. Keep
  // this path limited to extracted app libraries; accepting an inherited
  // private libc path would load a second Bionic provider and corrupt ART.
  const std::string bionic_library_path =
      app_library_directory.string() + ":" + app_data_directory.string() + "**";
  if (::setenv("BIONIC_LD_LIBRARY_PATH", bionic_library_path.c_str(), 1) != 0) {
    throw std::runtime_error("cannot configure Android app native-library path");
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
  prime_roblox_cookie_store(bootstrap_env);
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
  if (const char* override_path = ::getenv("NUAH_CONTENT_PATH");
      override_path && *override_path) {
    content_directory = override_path;
  } else {
    const auto sober_content =
        std::filesystem::path("/home/pepe/.var/app/org.vinegarhq.Sober/data/"
                              "sober/assets/content");
    content_directory = std::filesystem::is_directory(sober_content)
                            ? sober_content
                            : data_directory / "assets/content";
  }
  std::filesystem::create_directories(content_directory, data_error);
  if (data_error) {
    throw std::runtime_error("cannot create Roblox content directory: " +
                             data_error.message());
  }
  auto content_path = std::filesystem::absolute(content_directory).string();
  if (!content_path.ends_with('/')) content_path += '/';
  nuah_roblox_java_facade_set_content_path(content_path.c_str());

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
   * Accept a supplied response for real sessions; the empty application
   * settings document is only the offline diagnostic fallback. */
  report_bootstrap_stage("ROBLOX_CLIENT_SETTINGS_INIT");
  const char* settings_json = ::getenv("NUAH_CLIENT_SETTINGS_JSON");
  std::string settings_storage;
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
    settings_json = "{\"applicationSettings\":{}}";
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
    if (!native_gl || !start_game || !start_params || !update_surface_game ||
        !activity || !platform_params) {
      throw std::runtime_error(
          "Roblox direct game-start JNI contract is unavailable");
    }
    report_bootstrap_stage("ROBLOX_GAME_START");
    /* App/bootstrap initialization can rebuild the native HTTP client after
     * the early activity setup. Re-apply the authenticated Sober header at
     * the same boundary immediately before the join request. */
    prime_roblox_cookie_store(env);
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
      jobject global_native_gl = env->NewGlobalRef(native_gl);
      jobject global_start_params = env->NewGlobalRef(start_params);
      std::atomic<jint> async_result{0};
      std::thread starter([&] {
        JNIEnv* worker_env = nullptr;
        if (!vm || vm->AttachCurrentThread(&worker_env, nullptr) != JNI_OK ||
            !worker_env) {
          async_result.store(-1, std::memory_order_release);
          return;
        }
        async_result.store(start_game(worker_env,
                                      reinterpret_cast<jclass>(global_native_gl),
                                      global_start_params),
                           std::memory_order_release);
        vm->DetachCurrentThread();
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
      starter.join();
      start_result = async_result.load(std::memory_order_acquire);
      env->DeleteGlobalRef(global_native_gl);
      env->DeleteGlobalRef(global_start_params);
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
    if (explicit_surface_update) {
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
  while (!nuah_window_session_should_close(window.get())) {
    nuah_window_session_pump(window.get());
    (void)nuah_input_pump();
    ::usleep(10000);
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
  int status = 0;
  if (::waitpid(child, &status, 0) != child) {
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
  if (!std::filesystem::is_regular_file(options.apk)) {
    throw std::runtime_error("native APK does not exist: " +
                             options.apk.string());
  }
  if (options.width <= 0 || options.height <= 0) {
    throw std::runtime_error("native window dimensions must be positive");
  }

  if (!std::getenv("NUAH_DISABLE_SESSION_DISCOVERY")) {
    (void)discover_sober_session_cookie();
    (void)discover_webkit_user_id();
  }

  const auto image_apk = find_image(options);
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
  return run_nuah_jni_isolated(options, image_apk);
}

}  // namespace nuah
