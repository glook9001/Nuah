#include "nuah/atl_backend.hpp"
#include "nuah/apk_loader.hpp"

#include <gio/gio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>

#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace nuah {
namespace {
constexpr const char* kRobloxApplicationId = "com.roblox.client";

bool call_atl_action(const char* action, GVariant* parameter,
                     std::string& error) {
  GError* glib_error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr,
                                        &glib_error);
  if (!bus) {
    error = glib_error ? glib_error->message : "cannot open session bus";
    if (glib_error) g_error_free(glib_error);
    return false;
  }
  GVariantBuilder action_parameters;
  g_variant_builder_init(&action_parameters, G_VARIANT_TYPE("av"));
  g_variant_builder_add_value(&action_parameters,
                              g_variant_new_variant(parameter));
  GVariantBuilder platform_data;
  g_variant_builder_init(&platform_data, G_VARIANT_TYPE("a{sv}"));
  GVariant* reply = g_dbus_connection_call_sync(
      bus, kRobloxApplicationId, "/com/roblox/client", "org.gtk.Actions",
      "Activate",
      g_variant_new("(s@av@a{sv})", action,
                    g_variant_builder_end(&action_parameters),
                    g_variant_builder_end(&platform_data)),
      nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &glib_error);
  g_object_unref(bus);
  if (!reply) {
    error = glib_error ? glib_error->message
                       : "ATL action did not return a reply";
    if (glib_error) g_error_free(glib_error);
    return false;
  }
  g_variant_unref(reply);
  return true;
}

void discover_android16_runtime() {
  if (const char* configured = std::getenv("NUAH_ATL_RUNTIME");
      configured && std::string_view(configured) != "android16") {
    throw std::runtime_error(
        "Nuah supports only the ATL Android 16 runtime");
  }
  if (std::getenv("NUAH_ATL_RUNTIME")) {
    return;
  }
  if (std::getenv("NUAH_ATL_ANDROID16_HOME")) {
    if (::setenv("NUAH_ATL_RUNTIME", "android16", 1) != 0) {
      throw std::runtime_error(
          "cannot select the configured Android 16 ATL runtime");
    }
    return;
  }
  static const std::filesystem::path candidates[] = {
      "/usr/local/lib64/nuah/android16-runtime",
      "/usr/local/share/nuah/android16-runtime",
      "/opt/nuah/android16-runtime",
  };
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate /
                                         "android-translation-layer") &&
        std::filesystem::is_regular_file(candidate / "bootclasspath.txt") &&
        std::filesystem::is_regular_file(
            candidate / "natives/libtranslation_layer_main.so")) {
      if (::setenv("NUAH_ATL_RUNTIME", "android16", 1) != 0 ||
          ::setenv("NUAH_ATL_ANDROID16_HOME", candidate.c_str(), 1) != 0) {
        throw std::runtime_error(
            "cannot select the installed Android 16 ATL runtime");
      }
      return;
    }
  }
  throw std::runtime_error(
      "Nuah requires an installed ATL Android 16 runtime bundle");
}

std::string atl_executable() {
  const bool android16 = [] {
    const char* runtime = std::getenv("NUAH_ATL_RUNTIME");
    return runtime && std::string_view(runtime) == "android16";
  }();
  if (android16) {
    if (const char* configured = std::getenv("NUAH_ATL_ANDROID16_EXECUTABLE");
        configured && *configured) {
      return configured;
    }
    if (const char* home = std::getenv("NUAH_ATL_ANDROID16_HOME");
        home && *home) {
      return (std::filesystem::path(home) / "android-translation-layer")
          .string();
    }
    throw std::runtime_error(
        "NUAH_ATL_RUNTIME=android16 requires "
        "NUAH_ATL_ANDROID16_EXECUTABLE or NUAH_ATL_ANDROID16_HOME");
  }
  throw std::runtime_error(
      "Nuah supports only the ATL Android 16 runtime");
}

void install_legacy_overlay_preload() {
  const std::filesystem::path overlay = NUAH_ATL_OVERLAY_PATH;
  if (!std::filesystem::is_regular_file(overlay)) {
    throw std::runtime_error("Nuah ATL compatibility overlay is missing: " +
                             overlay.string());
  }
  std::string preload = overlay.string();
  if (const char* existing = std::getenv("LD_PRELOAD"); existing && *existing) {
    preload += ":";
    preload += existing;
  }
  if (::setenv("LD_PRELOAD", preload.c_str(), 1) != 0) {
    throw std::runtime_error("cannot preload Nuah ATL compatibility overlay");
  }
}

void install_atl_library_path() {
  /* Meson installs ATL's main JNI library below the Android dex tree, while
   * the standalone executable is installed in /usr/local/bin.  Relying on
   * ld.so to discover that sibling tree makes launches silently fail on a
   * clean host (and differs from running the build-tree binary). */
  const bool android16 = [] {
    const char* runtime = std::getenv("NUAH_ATL_RUNTIME");
    return runtime && std::string_view(runtime) == "android16";
  }();
  std::vector<std::filesystem::path> candidates;
  if (android16) {
    if (const char* configured = std::getenv("NUAH_ATL_ANDROID16_NATIVE_DIR");
        configured && *configured) {
      candidates.emplace_back(configured);
    } else if (const char* home = std::getenv("NUAH_ATL_ANDROID16_HOME");
               home && *home) {
      candidates.emplace_back(std::filesystem::path(home) / "natives");
    }
  } else if (const char* configured = std::getenv("NUAH_ATL_NATIVE_DIR");
             configured && *configured) {
    candidates.emplace_back(configured);
  }
  std::filesystem::path native_dir;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate /
                                         "libtranslation_layer_main.so")) {
      native_dir = candidate;
      break;
    }
  }
  if (native_dir.empty()) {
    throw std::runtime_error(
        "Nuah ATL native library directory is missing: "
        "libtranslation_layer_main.so");
  }

  std::string library_path = native_dir.string();
  if (android16) {
    // The Android 16 host ART adapter keeps ATL's JNI library in `natives/`
    // and ART's host shared objects in the sibling `lib/` directory.
    if (const char* home = std::getenv("NUAH_ATL_ANDROID16_HOME");
        home && *home) {
      library_path =
          (std::filesystem::path(home) / "lib").string() + ":" + library_path;
    }
  }
  if (const char* existing = std::getenv("LD_LIBRARY_PATH");
      existing && *existing) {
    library_path += ":";
    library_path += existing;
  }
  if (::setenv("LD_LIBRARY_PATH", library_path.c_str(), 1) != 0) {
    throw std::runtime_error("cannot set ATL library search path");
  }
}

void install_android16_runtime_environment(const std::string& data) {
  const char* runtime = std::getenv("NUAH_ATL_RUNTIME");
  if (!runtime || std::string_view(runtime) != "android16") return;

  const char* home = std::getenv("NUAH_ATL_ANDROID16_HOME");
  if ((!home || !*home) && !std::getenv("NUAH_ATL_ANDROID16_BOOTCLASSPATH")) {
    throw std::runtime_error(
        "Android 16 ATL runtime requires NUAH_ATL_ANDROID16_HOME or "
        "NUAH_ATL_ANDROID16_BOOTCLASSPATH");
  }

  if (!std::getenv("NUAH_ATL_ANDROID16_BOOTCLASSPATH") && home) {
    std::ifstream bootclasspath(
        std::filesystem::path(home) / "bootclasspath.txt");
    std::string value;
    std::getline(bootclasspath, value);
    if (value.empty()) {
      throw std::runtime_error(
          "Android 16 ATL runtime bootclasspath.txt is missing or empty");
    }
    if (::setenv("NUAH_ATL_ANDROID16_BOOTCLASSPATH", value.c_str(), 1) != 0) {
      throw std::runtime_error("cannot configure Android 16 bootclasspath");
    }
  }

  const char* bootclasspath =
      std::getenv("NUAH_ATL_ANDROID16_BOOTCLASSPATH");
  std::string resolved_bootclasspath;
  const std::filesystem::path runtime_home =
      home && *home ? std::filesystem::path(home) : std::filesystem::path();
  for (std::size_t start = 0; start <= std::strlen(bootclasspath);) {
    const std::string value(bootclasspath + start);
    const std::size_t separator = value.find(':');
    const std::string entry = value.substr(0, separator);
    if (!entry.empty()) {
      std::filesystem::path path(entry);
      if (path.is_relative() && !runtime_home.empty()) {
        path = runtime_home / path;
      }
      if (!resolved_bootclasspath.empty()) resolved_bootclasspath += ':';
      resolved_bootclasspath += path.string();
    }
    if (separator == std::string::npos) break;
    start += separator + 1;
  }
  if (resolved_bootclasspath.empty() ||
      ::setenv("BOOTCLASSPATH", resolved_bootclasspath.c_str(), 1) != 0 ||
      ::setenv("ANDROID_RUNTIME", "art", 1) != 0 ||
      ::setenv("ANDROID_DATA", data.c_str(), 1) != 0) {
    throw std::runtime_error("cannot configure Android 16 ART environment");
  }
  if (home && *home) {
    if (::setenv("ANDROID_ROOT", home, 1) != 0 ||
        ::setenv("ANDROID_ART_ROOT", home, 1) != 0) {
      throw std::runtime_error("cannot configure Android 16 ART root");
    }
  }
}

std::optional<std::filesystem::path> host_java_trust_store() {
  if (const char* configured = std::getenv("NUAH_JAVA_TRUST_STORE");
      configured && *configured) {
    const std::filesystem::path path = configured;
    if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error("configured Java trust store does not exist: " +
                               path.string());
    }
    return std::filesystem::canonical(path);
  }

  // wolfJSSE consumes a Java KeyStore, not a PEM bundle. Cover the standard
  // distro locations so ART receives the same maintained roots as host Java.
  static const std::filesystem::path candidates[] = {
      "/etc/pki/java/cacerts",          // Fedora/RHEL
      "/etc/ssl/certs/java/cacerts",    // Debian/Ubuntu
      "/etc/ssl/certs/java/cacerts.jks" // some Alpine installations
  };
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate)) {
      return std::filesystem::canonical(candidate);
    }
  }
  return std::nullopt;
}

std::filesystem::path default_data_directory() {
  if (const char* configured = std::getenv("XDG_DATA_HOME");
      configured && *configured) {
    return std::filesystem::path(configured) / "nuah";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".local/share/nuah";
  }
  return std::filesystem::temp_directory_path() /
         ("nuah-" + std::to_string(::getuid()));
}

std::optional<std::string> atl_jsse_bootclasspath() {
  std::vector<std::filesystem::path> roots;
  if (const char* home = std::getenv("NUAH_ATL_ANDROID16_HOME");
      home && *home) {
    const std::filesystem::path root(home);
    roots.push_back(root / "lib/java/dex/art");
    roots.push_back(root / "java/dex/art");
    roots.push_back(root / "dex/art");
  }
  // The distro ATL install keeps ART's hostdex jars in this shared directory.
  roots.emplace_back("/usr/local/lib64/java/dex/art");

  for (const auto& root : roots) {
    const auto wolfssl = root / "wolfssljni-hostdex.jar";
    const auto bouncycastle = root / "bouncycastle-hostdex.jar";
    if (std::filesystem::is_regular_file(wolfssl) &&
        std::filesystem::is_regular_file(bouncycastle)) {
      return wolfssl.string() + ":" + bouncycastle.string();
    }
  }
  return std::nullopt;
}

void write_member(const ApkMember& member,
                  const std::filesystem::path& destination) {
  const auto temporary = destination.string() + ".nuah-new";
  {
    std::ofstream output(temporary,
                         std::ios::binary | std::ios::trunc | std::ios::out);
    if (!output)
      throw std::runtime_error("cannot create extracted native library: " +
                               temporary);
    output.write(reinterpret_cast<const char*>(member.bytes.data()),
                 static_cast<std::streamsize>(member.bytes.size()));
    if (!output)
      throw std::runtime_error("cannot write extracted native library: " +
                               temporary);
  }
  std::filesystem::rename(temporary, destination);
}
}  // namespace

bool forward_atl_uri(const std::string& uri, std::string& error) {
  if (uri.empty()) {
    error = "cannot forward an empty Android URI";
    return false;
  }
  // The endpoint and method are implemented by GApplication. ATL registers
  // this name after the initial Android Activity exists; its `open` callback
  // then converts the URI to a fresh VIEW intent in that same ART process.
  GError* glib_error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr,
                                        &glib_error);
  if (!bus) {
    error = glib_error ? glib_error->message : "cannot open session bus";
    if (glib_error) g_error_free(glib_error);
    return false;
  }
  const char* uris[] = {uri.c_str(), nullptr};
  GVariantBuilder platform_data;
  g_variant_builder_init(&platform_data, G_VARIANT_TYPE("a{sv}"));
  GVariant* reply = g_dbus_connection_call_sync(
      bus, kRobloxApplicationId, "/com/roblox/client", "org.gtk.Application",
      "Open", g_variant_new("(^ass@a{sv})", uris, "",
                              g_variant_builder_end(&platform_data)),
      nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &glib_error);
  g_object_unref(bus);
  if (!reply) {
    error = glib_error ? glib_error->message
                       : "ATL URI forwarder could not reach the runtime";
    if (glib_error) g_error_free(glib_error);
    return false;
  }
  g_variant_unref(reply);
  return true;
}

bool forward_atl_cookie(const std::string& url, const std::string& value,
                        std::string& error) {
  if (url.empty() || value.empty()) {
    error = "cannot forward an empty Android WebView cookie";
    return false;
  }
  return call_atl_action("setCookie",
                         g_variant_new("(ss)", url.c_str(), value.c_str()),
                         error);
}

std::filesystem::path prepare_atl_native_libraries(
    const AtlLaunchOptions& options) {
  const auto data_directory =
      std::filesystem::absolute(options.data_directory.value_or(
          default_data_directory()));
  const auto app_directory =
      data_directory / (options.apk.filename().string() + "_");
  const auto library_directory = app_directory / "lib";
  std::filesystem::create_directories(library_directory);

  std::vector<std::filesystem::path> splits = options.split_apks;
  if (splits.empty()) {
    const auto sibling = options.apk.parent_path() / "split_config.x86_64.apk";
    if (std::filesystem::is_regular_file(sibling)) splits.push_back(sibling);
  }

  for (const auto& split : splits) {
    if (!std::filesystem::is_regular_file(split)) {
      throw std::runtime_error("ATL split APK does not exist: " +
                               split.string());
    }
    for (const auto& member :
         read_apk_members_with_prefix(split, "lib/x86_64/")) {
      const auto filename =
          std::filesystem::path(member.name).filename().string();
      if (filename.empty() || !filename.ends_with(".so")) continue;
      write_member(member, library_directory / filename);
    }
  }
  return data_directory;
}

[[noreturn]] void exec_atl(const AtlLaunchOptions& options) {
  if (!std::filesystem::is_regular_file(options.apk)) {
    throw std::runtime_error("ATL APK does not exist: " + options.apk.string());
  }
  if (options.width <= 0 || options.height <= 0) {
    throw std::runtime_error("ATL window dimensions must be positive");
  }

  discover_android16_runtime();
  const std::string executable = atl_executable();
  const std::string apk = std::filesystem::canonical(options.apk).string();
  const std::string width = std::to_string(options.width);
  const std::string height = std::to_string(options.height);
  const std::string data = prepare_atl_native_libraries(options).string();

  // A profile contains a single Android app-data tree.  Starting ATL twice
  // against it corrupts the ownership model of Roblox's SQLite/WAL cache and
  // makes the second runtime appear frozen while it waits for database locks.
  // Keep this descriptor open across exec; the flock is released when ATL
  // exits, including abnormal termination.
  const std::filesystem::path lock_path =
      std::filesystem::path(data) / ".nuah-atl-runtime.lock";
  const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
  if (lock_fd < 0) {
    throw std::runtime_error("cannot create ATL runtime lock: " +
                             lock_path.string());
  }
  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    ::close(lock_fd);
    if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
      throw std::runtime_error("ATL profile is already running: " + data);
    }
    throw std::runtime_error("cannot lock ATL profile: " + data);
  }
  if (::setenv("ANDROID_APP_DATA_DIR", data.c_str(), 1) != 0) {
    ::close(lock_fd);
    throw std::runtime_error("cannot set ATL application data directory");
  }
  // Keep Roblox/ATL temporary files with the profile.  Sharing /tmp with
  // unrelated Waydroid and analysis artifacts previously exhausted the tmpfs,
  // causing cache writes to fail and eventually turning into an OOM crash.
  const auto temporary_directory =
      std::filesystem::path(data) / "tmp";
  std::error_code temporary_error;
  std::filesystem::create_directories(temporary_directory, temporary_error);
  if (temporary_error ||
      ::setenv("TMPDIR", temporary_directory.c_str(), 1) != 0) {
    ::close(lock_fd);
    throw std::runtime_error("cannot configure ATL profile temporary directory");
  }
  // Roblox uses android.webkit.WebView for its desktop-visible login flow.
  // ATL ships a WebKitGTK backend but leaves it opt-in because lightweight
  // applications may not want to initialize a browser process.
  if (::setenv("ATL_UGLY_ENABLE_WEBVIEW", "1", 1) != 0) {
    throw std::runtime_error("cannot enable ATL WebKit backend");
  }
  // Present Roblox directly into the Wayland/X11 child surface. ATL's
  // fallback GdkGLTexture copy path creates a second GTK GL context and loses
  // it under Roblox's sustained render loop, producing an audio-only window.
  if (::setenv("ATL_DIRECT_EGL", "1", 1) != 0) {
    throw std::runtime_error("cannot enable ATL direct EGL presentation");
  }
  /* Mesa otherwise honors Roblox's Android preference for immediate WSI
   * presentation (mode 0).  Mailbox is the supported low-latency desktop
   * alternative on this Mesa stack: it avoids tearing while allowing the
   * compositor to discard stale frames.  Keep an explicit override available
   * for diagnostics and compatibility testing. */
  if (!std::getenv("MESA_VK_WSI_PRESENT_MODE") &&
      ::setenv("MESA_VK_WSI_PRESENT_MODE", "mailbox", 1) != 0) {
    throw std::runtime_error("cannot enable low-latency Vulkan presentation");
  }
  install_atl_library_path();
  install_android16_runtime_environment(data);
  // Sober resolves its Android namespace inside the protected runtime and
  // does not rely on LD_PRELOAD. Keep the old overlay only as an explicit
  // diagnostic escape hatch while Nuah's registry is being completed.
  if (const char* legacy = std::getenv("NUAH_ENABLE_LEGACY_PRELOAD");
      legacy && std::string_view(legacy) == "1") {
    install_legacy_overlay_preload();
  }

  // ATL is deliberately non-unique while bootstrapping, so it needs an
  // explicit app ID to own the session-bus endpoint used for later URI opens.
  std::vector<std::string> arguments{executable, "--gapplication-app-id",
                                     kRobloxApplicationId, apk, "-w", width,
                                     "-h", height};
  if (const auto trust_store = host_java_trust_store()) {
    arguments.emplace_back("-X");
    arguments.push_back("-Djavax.net.ssl.trustStore=" + trust_store->string());
    arguments.emplace_back("-X");
    arguments.emplace_back("-Djavax.net.ssl.trustStoreType=JKS");
  }
  // Android's security.properties names WolfSSLProvider and BouncyCastle, but
  // the standalone ATL launcher only boots core-oj/core-libart.  Append the
  // already-installed hostdex provider jars so TrustManagerFactory.PKIX can
  // resolve without adding a second JVM or compiling a provider.
  if (const auto jsse = atl_jsse_bootclasspath()) {
    arguments.emplace_back("-X");
    arguments.push_back("-Xbootclasspath/a:" + *jsse);
  }
  if (options.activity && !options.activity->empty()) {
    arguments.emplace_back("-l");
    arguments.push_back(*options.activity);
  }
  if (options.uri && !options.uri->empty()) {
    arguments.emplace_back("--uri");
    arguments.push_back(*options.uri);
  }

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (auto& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);

  ::execvp(executable.c_str(), argv.data());
  throw std::runtime_error("cannot execute full Android Translation Layer: " +
                           executable);
}

}  // namespace nuah
