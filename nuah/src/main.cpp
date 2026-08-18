#include "nuah/atl_backend.hpp"
#include "nuah/launch_uri.hpp"
#include "nuah/protocol.hpp"
#include "nuah/sober_cache.hpp"

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <glib.h>
#include <iostream>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex service_write_mutex;

void reexec_with_bundled_pthread_bridge(char** argv) {
  /* libhybris already forwards Android pthread/TLS calls into the process's
   * one host libc domain.  Re-execing with Nuah's compatibility DSO adds a
   * second wrapper layer and showed up as avoidable mutex contention in the
   * heavy-room profile.  Keep that DSO available for ABI comparison and old
   * hosts, but make the single-domain libhybris path the product default. */
  const char* requested = std::getenv("NUAH_USE_BUNDLED_PTHREAD_BRIDGE");
  if (!requested || std::strcmp(requested, "0") == 0) return;
  if (const char* ready = std::getenv("NUAH_PTHREAD_BRIDGE_READY");
      ready && std::strcmp(ready, "1") == 0) {
    return;
  }

  const auto executable = std::filesystem::canonical("/proc/self/exe");
  std::vector<std::filesystem::path> candidates;
  if (const char* configured = std::getenv("NUAH_BIONIC_TRANSLATION_DIR");
      configured && *configured) {
    candidates.emplace_back(configured);
  }
  candidates.emplace_back(executable.parent_path() / "bionic-translation");
  candidates.emplace_back(executable.parent_path() / "lib/nuah/bionic-translation");

  std::filesystem::path selected;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate / "libpthread_bio.so.0")) {
      selected = candidate;
      break;
    }
  }
  if (selected.empty()) {
    if (const char* trace = std::getenv("NUAH_PERF_TRACE");
        trace && *trace && std::strcmp(trace, "0") != 0) {
      std::cerr << "nuah performance: bundled pthread bridge unavailable; "
                   "using host fallback\n";
    }
    return;
  }

  std::string library_path = selected.string();
  if (const char* existing = std::getenv("LD_LIBRARY_PATH");
      existing && *existing) {
    library_path += ':';
    library_path += existing;
  }
  if (::setenv("LD_LIBRARY_PATH", library_path.c_str(), 1) != 0 ||
      ::setenv("NUAH_PTHREAD_BRIDGE_READY", "1", 1) != 0) {
    throw std::runtime_error("cannot select Nuah's pthread bridge");
  }
  if (const char* trace = std::getenv("NUAH_PERF_TRACE");
      trace && *trace && std::strcmp(trace, "0") != 0) {
    std::cerr << "nuah performance: pthread bridge="
              << (selected / "libpthread_bio.so.0") << '\n';
  }
  ::execv(executable.c_str(), argv);
  throw std::runtime_error("cannot restart Nuah with its pthread bridge");
}

bool send_service_control(int fd, std::uint8_t opcode,
                          const std::string& payload, std::string& error) {
  std::lock_guard lock(service_write_mutex);
  return nuah::send_sober_control(fd, opcode, payload, error);
}

std::string supervisor_data_directory() {
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    return (std::filesystem::path(xdg) / "nuah").string();
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return (std::filesystem::path(home) / ".local/share/nuah").string();
  }
  return (std::filesystem::temp_directory_path() / "nuah").string();
}


int start_services(const char* argv0) {
  const auto executable = std::filesystem::canonical(argv0);
  const auto services = executable.parent_path() / "nuah-services";
  if (!std::filesystem::is_regular_file(services)) throw std::runtime_error("nuah-services is not installed beside nuah");
  int pair[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
    throw std::runtime_error("cannot create Nuah Services control socket");
  }
  const auto child = ::fork();
  if (child < 0) throw std::runtime_error("cannot start Nuah Services");
  if (child == 0) {
    ::close(pair[0]);
    const auto fd = std::to_string(pair[1]);
    ::execl(services.c_str(), services.c_str(), "--server", fd.c_str(),
            "attached", static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(pair[1]);
  // This parent is the runtime-supervisor boundary.  It deliberately owns
  // the child and the socket for the full Services lifetime, matching Sober's
  // architecture rather than returning after a detached GTK spawn.
  std::string error;
  if (!send_service_control(pair[0], nuah::kSoberLoadUri,
                            "https://www.roblox.com/home", error)) {
    ::close(pair[0]);
    ::kill(child, SIGTERM);
    ::waitpid(child, nullptr, 0);
    throw std::runtime_error(error);
  }
  const auto cache = nuah::inspect_sober_cache();
  // The ATL process can outlive (or lose) its D-Bus application endpoint.
  // Keep the PID in shared atomic state because the waiter runs on a helper
  // thread and must never clear the PID of a newer runtime instance.
  auto runtime = std::make_shared<std::atomic<pid_t>>(-1);
  std::string pending_cookie_header;
  auto install_cookie = [&](const std::string& header, std::string& detail) {
    for (int attempt = 0; attempt < 150; ++attempt) {
      if (nuah::forward_atl_cookie("https://www.roblox.com/", header,
                                   detail)) {
        return true;
      }
      ::usleep(100000);
    }
    return false;
  };
  auto send_service_command = [&](std::uint8_t opcode,
                                  const std::string& payload) {
    std::string service_error;
    if (!send_service_control(pair[0], opcode, payload, service_error)) {
      std::cerr << "nuah supervisor: cannot control Services window: "
                << service_error << '\n';
      return false;
    }
    return true;
  };
  auto start_runtime = [&](const std::string& uri) -> bool {
    if (!cache.structurally_valid) {
      std::cerr << "nuah supervisor: cannot start Roblox: " << cache.message << '\n';
      return false;
    }
    const pid_t live_runtime = runtime->load();
    if (live_runtime > 0 && ::kill(live_runtime, 0) == 0) {
      std::string forward_error;
      if (uri.empty()) return true;
      if (!nuah::forward_atl_uri(uri, forward_error)) {
        std::cerr << "nuah supervisor: Roblox runtime is already running";
        if (!forward_error.empty()) std::cerr << ": " << forward_error;
        std::cerr << '\n';
      } else {
        std::cerr << "nuah supervisor: delivered URI to live Roblox runtime\n";
        return true;
      }
      return false;
    }
    const pid_t spawned_runtime = ::fork();
    if (spawned_runtime < 0) {
      std::cerr << "nuah supervisor: cannot fork Roblox runtime\n";
      return false;
    }
    if (spawned_runtime == 0) {
      const auto apk = cache.base_apk.string();
      const auto split = cache.split_apk.string();
      const auto data = supervisor_data_directory();
      if (!pending_cookie_header.empty()) {
        ::setenv("NUAH_ROBLOX_COOKIE_HEADER", pending_cookie_header.c_str(), 1);
        ::setenv("NUAH_ROBLOX_COOKIES", pending_cookie_header.c_str(), 1);
      }
      if (!::getenv("NUAH_CLIENT_SETTINGS_JSON")) {
        ::setenv("NUAH_CLIENT_SETTINGS_JSON",
                 "{\"applicationSettings\":{\"DFFlagDebugDisableRbxTransportDummyClient\":true}}", 1);
      }
      if (!::getenv("NUAH_ART_LIBRARY_DIR")) {
        ::setenv("NUAH_ART_LIBRARY_DIR", "/usr/local/lib64/art", 1);
      }
      if (!::getenv("NUAH_ATL_ANDROID16_HOME")) {
        ::setenv("NUAH_ATL_ANDROID16_HOME", "/usr/local/lib64/java/dex/art", 1);
      }
      if (!::getenv("NUAH_ATL_HOME")) {
        const std::string sys_atl = "/usr/local/lib64/java/dex/android_translation_layer";
        if (std::filesystem::is_regular_file(sys_atl + "/framework-res.apk")) {
          ::setenv("NUAH_ATL_HOME", sys_atl.c_str(), 1);
        } else {
          const auto dist_atl = (executable.parent_path().parent_path() / "dist/java/dex/android_translation_layer").string();
          if (std::filesystem::is_regular_file(dist_atl + "/framework-res.apk")) {
            ::setenv("NUAH_ATL_HOME", dist_atl.c_str(), 1);
          }
        }
      }
      if (!::getenv("NUAH_ATL_NATIVE_DIR")) {
        const std::string atl_native = (std::filesystem::path(data) / "base.apk_/lib").string();
        ::setenv("NUAH_ATL_NATIVE_DIR", atl_native.c_str(), 1);
      }
      if (!::getenv("NUAH_HYBRIS_LIBRARY")) {
        const std::string hybris_lib = (std::filesystem::path(data) / "hybris/lib/libhybris-common.so").string();
        ::setenv("NUAH_HYBRIS_LIBRARY", hybris_lib.c_str(), 1);
      }
      if (!::getenv("HYBRIS_LINKER_DIR")) {
        const std::string hybris_linker = (std::filesystem::path(data) / "hybris/lib/libhybris/linker").string();
        ::setenv("HYBRIS_LINKER_DIR", hybris_linker.c_str(), 1);
      }
      if (!::getenv("NUAH_GRAPHICS_BACKEND")) {
        ::setenv("NUAH_GRAPHICS_BACKEND", "vulkan", 1);
      }
      if (!::getenv("NUAH_VULKAN_PRESENT_MODE")) {
        ::setenv("NUAH_VULKAN_PRESENT_MODE", "fifo", 1);
      }
      const char* existing_preload = ::getenv("LD_PRELOAD");
      std::string preload = "/usr/lib64/libpng16.so.16:/usr/lib64/libjpeg.so.62:/usr/local/lib64/art/libandroidfw.so";
      if (existing_preload && *existing_preload) {
        preload = std::string(existing_preload) + ":" + preload;
      }
      ::setenv("LD_PRELOAD", preload.c_str(), 1);

      const char* existing_ld = ::getenv("LD_LIBRARY_PATH");
      std::string ld_path = "/usr/local/lib64/art:/usr/local/lib64/art/natives:/usr/local/lib64/java/dex/art/natives:" +
                            (std::filesystem::path(data) / "hybris/lib").string() + ":" +
                            (std::filesystem::path(data) / "base.apk_/lib").string();
      if (existing_ld && *existing_ld) {
        ld_path = std::string(existing_ld) + ":" + ld_path;
      }
      ::setenv("LD_LIBRARY_PATH", ld_path.c_str(), 1);

      if (uri.empty()) {
        ::execl(executable.c_str(), executable.c_str(), "native-run", "--apk",
                apk.c_str(), "--split", split.c_str(), "--data", data.c_str(),
                static_cast<char*>(nullptr));
      } else {
        ::execl(executable.c_str(), executable.c_str(), "native-run", "--apk",
                apk.c_str(), "--split", split.c_str(), "--data", data.c_str(),
                "--uri", uri.c_str(), static_cast<char*>(nullptr));
      }
      _exit(127);
    }
    runtime->store(spawned_runtime);
    std::cerr << "nuah supervisor: started Roblox runtime pid=" << spawned_runtime << '\n';
    // Sober keeps Services as the supervised WebKit/control helper, not as a
    // competing desktop window once the Android client owns the interaction.
    // Hiding rather than terminating it preserves the private bridge and its
    // WebKit session for a later explicit return to Services.
    send_service_command(nuah::kSoberSetVisible, std::string(1, '\0'));
    if (!pending_cookie_header.empty()) {
      std::string cookie_error;
      if (!install_cookie(pending_cookie_header, cookie_error)) {
        std::cerr << "nuah supervisor: Android session handoff failed: "
                  << cookie_error << '\n';
      }
    }
    const int service_fd = pair[0];
    const pid_t watched_runtime = spawned_runtime;
    std::thread([service_fd, watched_runtime, runtime] {
      int runtime_status = 0;
      while (::waitpid(watched_runtime, &runtime_status, 0) < 0 &&
             errno == EINTR) {
      }
      pid_t expected = watched_runtime;
      runtime->compare_exchange_strong(expected, -1);
      std::string show_error;
      if (!send_service_control(service_fd, nuah::kSoberSetVisible,
                                std::string(1, '\1'), show_error)) {
        std::cerr << "nuah supervisor: cannot restore Services window: "
                  << show_error << '\n';
      }
    }).detach();
    return true;
  };
  auto hybrid_callback = [&](const std::string& callback_id, bool accepted,
                             const char* detail) {
    if (callback_id.empty()) return;
    const std::string script =
        "(()=>{const b=window.Roblox?.Hybrid?.Bridge;"
        "if(b&&typeof b.nativeCallback==='function')b.nativeCallback('" +
        callback_id + "'," + (accepted ? "0" : "1") +
        ",{accepted:" + (accepted ? "true" : "false") +
        ",detail:'" + detail + "'});})()";
    if (!send_service_command(nuah::kSoberEvaluateJavaScript, script)) {
      std::cerr << "nuah supervisor: cannot return Hybrid callback\n";
    }
  };
  while (true) {
    nuah::SoberRecord event{};
    if (!nuah::receive_sober_record(pair[0], event, error)) break;
      if (event.opcode == nuah::kSoberWebKitMessage) {
      const bool is_session_cookie =
          event.payload.find(R"("type":"web.session_cookie")") !=
          std::string::npos;
      std::cerr << "nuah supervisor: "
                << (is_session_cookie
                        ? R"({"type":"web.session_cookie","redacted":true})"
                        : event.payload)
                << '\n';
      if (event.payload.find(R"("type":"runtime.open")") != std::string::npos) {
        start_runtime({});
      } else if (event.payload.find(R"("type":"runtime.uri")") !=
                 std::string::npos) {
        static const std::regex uri_pattern(
            "\\\"uri\\\"\\s*:\\s*\\\"([^\\\"\\\\]+)\\\"");
        std::smatch uri;
        if (!std::regex_search(event.payload, uri, uri_pattern)) {
          std::cerr << "nuah supervisor: invalid Roblox navigation event\n";
        } else {
          try {
            const auto request = nuah::parse_roblox_uri(uri[1].str());
            start_runtime(nuah::format_roblox_uri(request));
          } catch (const std::exception& launch_error) {
            std::cerr << "nuah supervisor: invalid Roblox navigation: "
                      << launch_error.what() << '\n';
          }
        }
      } else if (is_session_cookie) {
        static const std::regex cookie_pattern(
            "\\\"value_b64\\\"\\s*:\\s*\\\"([A-Za-z0-9+/=]+)\\\"");
        std::smatch encoded;
        bool accepted = false;
        if (std::regex_search(event.payload, encoded, cookie_pattern)) {
          gsize value_length = 0;
          guchar* decoded = g_base64_decode(encoded[1].str().c_str(),
                                            &value_length);
          std::string cookie(reinterpret_cast<char*>(decoded), value_length);
          g_free(decoded);
          // Cookie header values must never contain a new line.  This also
          // keeps the Android CookieManager handoff to one exact header.
          if (cookie.find_first_of("\r\n") == std::string::npos &&
              !cookie.empty()) {
            std::string cookie_error;
            const std::string header =
                ".ROBLOSECURITY=" + cookie +
                "; Domain=.roblox.com; Path=/; Secure; HttpOnly";
            // Services owns login; do not start the graphics runtime merely
            // because the user authenticated.  The header is installed after
            // the real game runtime is created for a join URI.
            pending_cookie_header = header;
            accepted = true;
            try {
              const auto cookies_file =
                  std::filesystem::path(supervisor_data_directory()) / "cookies";
              std::ofstream out(cookies_file, std::ios::trunc);
              if (out.is_open()) {
                out << ".ROBLOSECURITY=" << cookie << '\n';
              }
            } catch (...) {
            }
            if (runtime->load() > 0 && ::kill(runtime->load(), 0) == 0) {
              accepted = install_cookie(pending_cookie_header, cookie_error);
            }
            std::cerr << "nuah supervisor: authenticated session stored for game launch\n";
          }
        }
      } else if (event.payload.find(R"("type":"web.session_clear")") !=
                 std::string::npos) {
        pending_cookie_header.clear();
        std::error_code ec;
        std::filesystem::remove(
            std::filesystem::path(supervisor_data_directory()) / "cookies", ec);
        std::cerr << "nuah supervisor: session cleared on logout\n";
      } else if (event.payload.find(R"("moduleID")") != std::string::npos) {
        static const std::regex module_pattern(
            "\\\"moduleID\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
        static const std::regex function_pattern(
            "\\\"functionName\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
        static const std::regex callback_pattern(
            "\\\"callbackID\\\"\\s*:\\s*\\\"([A-Za-z0-9-]{1,128})\\\"");
        static const std::regex launch_place_pattern(
            "\\\"placeId\\\"\\s*:\\s*\\\"?([0-9]+)\\\"?");
        static const std::regex start_place_pattern(
            "\\\"placeID\\\"\\s*:\\s*\\\"?([0-9]+)\\\"?");
        static const std::regex instance_pattern(
            "\\\"gameInstanceId\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
        static const std::regex reserved_pattern(
            "\\\"reservedServerAccessCode\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
        static const std::regex private_link_pattern(
            "\\\"privateServerLinkCode\\\"\\s*:");
        static const std::regex follow_user_pattern("\\\"playerId\\\"\\s*:");
        std::smatch module;
        std::smatch function;
        std::smatch callback;
        std::smatch match;
        const std::string callback_id =
            std::regex_search(event.payload, callback, callback_pattern)
                ? callback[1].str()
                : "";
        if (!std::regex_search(event.payload, module, module_pattern) ||
            !std::regex_search(event.payload, function, function_pattern) ||
            module[1].str() != "Game" ||
            (function[1].str() != "launchGame" &&
             function[1].str() != "startWithPlaceID")) {
          hybrid_callback(callback_id, false,
                          "unsupported Hybrid method");
        } else if (std::regex_search(event.payload, private_link_pattern) ||
                   std::regex_search(event.payload, follow_user_pattern)) {
          // These are part of the browser's Game request grammar, but no
          // evidence yet establishes their conversion to Android's deep-link
          // names.  Reject rather than misroute a private/follow-user join.
          hybrid_callback(callback_id, false,
                          "private and follow-user joins need Android mapping");
        } else {
          const bool is_start_with_place = function[1].str() == "startWithPlaceID";
          const auto& place_pattern =
              is_start_with_place ? start_place_pattern : launch_place_pattern;
          if (!std::regex_search(event.payload, match, place_pattern)) {
            hybrid_callback(callback_id, false,
                            "game request has no place ID");
            continue;
          }
          nuah::LaunchRequest request{};
          request.place_id = match[1].str();
          std::smatch instance;
          std::smatch reserved;
          if (std::regex_search(event.payload, instance, instance_pattern)) {
            request.game_instance_id = instance[1].str();
          }
          if (std::regex_search(event.payload, reserved, reserved_pattern)) {
            request.reserved_server_access_code = reserved[1].str();
          }
          bool accepted = false;
          try {
            accepted = start_runtime(nuah::format_roblox_uri(request));
          } catch (const std::exception& launch_error) {
            std::cerr << "nuah supervisor: invalid Hybrid launch: "
                      << launch_error.what() << '\n';
          }
          hybrid_callback(callback_id, accepted,
                          accepted ? "launch request delivered"
                                   : "runtime launch failed");
        }
      }
    }
  }
  ::close(pair[0]);
  const pid_t active_runtime = runtime->exchange(-1);
  if (active_runtime > 0) {
    ::kill(active_runtime, SIGTERM);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) < 0 || !WIFEXITED(status)) {
    throw std::runtime_error("Nuah Services terminated unexpectedly");
  }
  if (WEXITSTATUS(status) != 0) {
    throw std::runtime_error("Nuah Services exited " + std::to_string(WEXITSTATUS(status)));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc >= 2 && (std::string(argv[1]) == "atl-run" ||
                      std::string(argv[1]) == "native-run")) {
      nuah::AtlLaunchOptions options;
      for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--apk") options.apk = value;
        else if (key == "--split") options.split_apks.emplace_back(value);
        else if (key == "--activity") options.activity = value;
        else if (key == "--uri") options.uri = value;
        else if (key == "--data") options.data_directory = value;
        else if (key == "--width") {
          options.width = std::stoi(value);
          options.dimensions_explicit = true;
        }
        else if (key == "--height") {
          options.height = std::stoi(value);
          options.dimensions_explicit = true;
        }
        else throw std::runtime_error("unknown ATL option: " + key);
      }
      if (options.apk.empty()) {
        throw std::runtime_error(
            "usage: nuah atl-run --apk <file.apk> [--activity <class>] "
            "[--split <config.apk>] [--uri <uri>] [--data <directory>] "
            "[--width <px>] [--height <px>]");
      }
      if (std::string(argv[1]) == "native-run") {
        reexec_with_bundled_pthread_bridge(argv);
        return nuah::run_native(options);
      }
      nuah::exec_atl(options);
    }
    if (argc == 2 && std::string(argv[1]) == "sober-cache-status") {
      const auto status = nuah::inspect_sober_cache();
      std::cout << "source=" << status.source << " present=" << status.present
                << " structurally_valid=" << status.structurally_valid << "\n" << status.message << '\n';
      return status.structurally_valid ? 0 : 1;
    }
    if (argc == 3 && std::string(argv[1]) == "adopt-sober-cache") {
      const auto status = nuah::adopt_sober_cache(argv[2]);
      std::cout << status.message << '\n';
      return status.structurally_valid ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "config") return start_services(argv[0]);
    std::cerr << "usage: nuah config | nuah atl-run --apk <file.apk> [--activity <class>] [--split <config.apk>] [--uri <uri>] [--data <directory>] [--width <px>] [--height <px>] | nuah sober-cache-status | nuah adopt-sober-cache <directory>\n";
    return 2;
  } catch (const std::exception& error) { std::cerr << "nuah: " << error.what() << '\n'; return 1; }
}
