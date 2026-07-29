#include "nuah/atl_backend.hpp"
#include "nuah/apk_loader.hpp"
#include "nuah/bootstrap_diagnostics.h"
#include "nuah/input_bridge.h"
#include "nuah/native_session.h"
#include "nuah/nuah_jvm.h"
#include "nuah/window_session.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <jni.h>

namespace nuah {
namespace {

extern "C" void nuah_roblox_java_facade_set_content_path(const char* path);

void report_bootstrap_stage(const char* stage) {
  nuah_bootstrap_diagnostics_set_stage(stage);
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

int run_nuah_jni(const NativeLaunchOptions& options,
                 const std::filesystem::path& apk) {
  std::string asset_apks;
  for (const auto& candidate : image_candidates(options)) {
    if (!std::filesystem::is_regular_file(candidate)) continue;
    if (!asset_apks.empty()) asset_apks += ':';
    asset_apks += std::filesystem::absolute(candidate).string();
  }
  if (asset_apks.empty() || ::setenv("NUAH_APK_PATHS", asset_apks.c_str(), 1) != 0) {
    throw std::runtime_error("cannot configure Android asset APK paths");
  }
  report_bootstrap_stage("ANDROID_DLOPEN_CONSTRUCTORS");
  auto image = load_apk_library(apk, "lib/x86_64/libroblox.so");
  report_bootstrap_stage("JNI_ONLOAD");
  std::unique_ptr<NuahNativeSession, decltype(&nuah_native_session_destroy)>
      session(nuah_native_session_create(), nuah_native_session_destroy);
  if (!session) throw std::runtime_error("cannot create Nuah native session");
  NuahJvm* jvm = nuah_native_session_jvm(session.get());
  const auto on_load = reinterpret_cast<jint (*)(JavaVM*, void*)>(
      image.symbol("JNI_OnLoad"));
  if (!on_load) throw std::runtime_error("libroblox.so does not export JNI_OnLoad");
  const jint version = on_load(reinterpret_cast<JavaVM*>(nuah_jvm_java_vm(jvm)), nullptr);
  if (version == JNI_ERR || version == JNI_EVERSION) {
    throw std::runtime_error("libroblox.so JNI_OnLoad rejected Nuah JVM with status " +
                             std::to_string(version));
  }

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
  if (!nuah_jvm_find_registered_native(
          jvm, kGameActivity, "initializeNativeCode", kInitializeSignature)) {
    void* exported_initialize = image.symbol(
        "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
    if (!exported_initialize ||
        !nuah_jvm_bind_native(jvm, kGameActivity, "initializeNativeCode",
                              kInitializeSignature, exported_initialize)) {
      throw std::runtime_error(
          "libroblox.so exposes neither a registered nor exported "
          "GameActivity initializeNativeCode");
    }
  }
  struct NativeRequirement {
    const char* member;
    const char* signature;
  };
  constexpr NativeRequirement kInputLifecycleRequirements[] = {
      {"onStartNative", "(J)V"},
      {"onResumeNative", "(J)V"},
      {"onPauseNative", "(J)V"},
      {"onStopNative", "(J)V"},
      {"onSurfaceCreatedNative", "(JLandroid/view/Surface;)V"},
      {"onSurfaceChangedNative", "(JLandroid/view/Surface;III)V"},
      {"onSurfaceDestroyedNative", "(J)V"},
      {"onTouchEventNative", "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z"},
      {"onKeyDownNative", "(JLandroid/view/KeyEvent;)Z"},
      {"onKeyUpNative", "(JLandroid/view/KeyEvent;)Z"},
      {"onTextInputEventNative",
       "(JLcom/google/androidgamesdk/gametextinput/State;)V"},
  };
  std::unique_ptr<NuahWindowSession, decltype(&nuah_window_session_destroy)>
      window(nuah_window_session_create(options.width, options.height, "Roblox"),
             nuah_window_session_destroy);
  if (!window) throw std::runtime_error("cannot create Nuah SDL/Vulkan window");
  void* surface = nuah_native_session_surface(
      session.get(), nuah_window_session_native_window(window.get()));
  if (!surface) throw std::runtime_error("cannot create Nuah Android Surface façade");

  const auto data_directory = options.data_directory.value_or(
      std::filesystem::temp_directory_path() / "nuah-data");
  std::error_code data_error;
  std::filesystem::create_directories(data_directory, data_error);
  if (data_error) {
    throw std::runtime_error("cannot create Nuah game data directory: " +
                             data_error.message());
  }
  report_bootstrap_stage("GAMEACTIVITY_INITIALIZE");
  if (!nuah_native_session_initialize_game(session.get(), "com.roblox.client",
                                           data_directory.c_str())) {
    throw std::runtime_error("GameActivity initializeNativeCode returned no native handle");
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
  jclass init_params_class =
      env->FindClass("com/roblox/engine/jni/autovalue/AutoValue_InitParams");
  jobject init_params =
      init_params_class ? env->AllocObject(init_params_class) : nullptr;
  using SetAssetPath = void (*)(JNIEnv*, jclass, jstring);
  using SetInitParams = void (*)(JNIEnv*, jclass, jobject);
  const auto set_asset_path = reinterpret_cast<SetAssetPath>(image.symbol(
      "Java_com_roblox_client_startup_MainGameActivity_nativeSetAssetPath"));
  const auto set_init_params = reinterpret_cast<SetInitParams>(image.symbol(
      "Java_com_roblox_client_startup_MainGameActivity_"
      "nativeAppBridgeSetInitParams"));
  if (!main_activity || !init_params || !set_asset_path || !set_init_params) {
    throw std::runtime_error(
        "Roblox MainGameActivity pre-start JNI contract is unavailable");
  }
  report_bootstrap_stage("ROBLOX_ASSET_PATH");
  set_asset_path(env, main_activity, env->NewStringUTF(content_path.c_str()));
  report_bootstrap_stage("ROBLOX_INIT_PARAMS");
  set_init_params(env, main_activity, init_params);

  for (const auto& requirement : kInputLifecycleRequirements) {
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
  // Roblox installs its own native crash handlers during initialization.
  // Re-arm the supervisor's last-chance recorder at the lifecycle boundary so
  // a fatal worker-thread fault still reports an ELF-relative caller.
  nuah_bootstrap_diagnostics_install_signal_handler();
  report_bootstrap_stage("GAMEACTIVITY_START");
  if (!nuah_native_session_dispatch_lifecycle(session.get(), "onStartNative")) {
    throw std::runtime_error("GameActivity start callback is unavailable");
  }
  report_bootstrap_stage("GAMEACTIVITY_RESUME");
  if (!nuah_native_session_dispatch_lifecycle(session.get(), "onResumeNative")) {
    throw std::runtime_error("GameActivity resume callback is unavailable");
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
  std::cerr << "nuah native: libroblox.so accepted retained Nuah JVM JNI version 0x"
            << std::hex << version << std::dec << '\n';
  return 0;
}

int run_nuah_jni_isolated(const NativeLaunchOptions& options,
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
  const pid_t child = ::fork();
  if (child < 0) {
    nuah_bootstrap_diagnostics_attach(nullptr);
    ::munmap(mapping, sizeof(*diagnostics));
    throw std::runtime_error("cannot start isolated native bootstrap");
  }
  if (child == 0) {
    nuah_bootstrap_diagnostics_install_signal_handler();
    try {
      _exit(run_nuah_jni(options, apk));
    } catch (const std::exception& error) {
      std::cerr << "nuah bootstrap: native initialization failed before JNI_OnLoad: "
                << error.what() << '\n';
      _exit(70);
    }
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
