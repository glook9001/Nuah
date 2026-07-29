#include "nuah/bootstrap_diagnostics.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "bootstrap diagnostics test: %s\n", message);
  return 1;
}

}  // namespace

int main() {
  void* mapping =
      ::mmap(nullptr, sizeof(NuahBootstrapDiagnostics),
             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) return fail("mmap failed");
  auto* diagnostics = static_cast<NuahBootstrapDiagnostics*>(mapping);
  std::memset(diagnostics, 0, sizeof(*diagnostics));
  diagnostics->version = 1;
  nuah_bootstrap_diagnostics_attach(diagnostics);
  nuah_bootstrap_diagnostics_set_stage("ANDROID_DLOPEN_CONSTRUCTORS");

  void* bionic = ::dlopen(NUAH_TEST_BIONIC_PATH, RTLD_NOW | RTLD_LOCAL);
  if (!bionic) return fail(::dlerror());
  const auto setter = reinterpret_cast<void (*)(
      const NuahDiagnosticsCallbacks*)>(
      ::dlsym(bionic, "nuah_bionic_set_diagnostics_callbacks"));
  const auto set_abort_message =
      reinterpret_cast<void (*)(const char*)>(
          ::dlsym(bionic, "android_set_abort_message"));
  const auto property_get =
      reinterpret_cast<int (*)(const char*, char*)>(
          ::dlsym(bionic, "__system_property_get"));
  const auto android_abort =
      reinterpret_cast<void (*)()>(::dlsym(bionic, "abort"));
  auto* android_stderr =
      reinterpret_cast<std::FILE**>(::dlsym(bionic, "stderr"));
  if (!setter || !set_abort_message || !property_get || !android_abort) {
    return fail("required libbionic diagnostic symbols are unavailable");
  }
  if (!android_stderr || *android_stderr != stderr) {
    return fail("Android stderr does not reference the host-owned stream");
  }
  const auto callbacks = nuah_bootstrap_diagnostics_callbacks();
  setter(&callbacks);

  const pid_t child = ::fork();
  if (child < 0) return fail("fork failed");
  if (child == 0) {
    char value[128]{};
    property_get("ro.build.version.sdk", value);
    set_abort_message("fixture constructor rejected its environment");
    android_abort();
    _exit(99);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child) return fail("waitpid failed");
  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
    return fail("wrapped Android abort did not terminate with SIGABRT");
  }
  if (!diagnostics->abort_seen || !diagnostics->caller ||
      !diagnostics->module_path[0]) {
    return fail("abort caller was not normalized to a mapped module");
  }
  if (std::strcmp(diagnostics->stage, "ANDROID_DLOPEN_CONSTRUCTORS") != 0) {
    return fail("bootstrap stage was not retained");
  }
  if (std::strcmp(diagnostics->abort_message,
                  "fixture constructor rejected its environment") != 0) {
    return fail("Android abort message was not retained");
  }
  if (std::strcmp(diagnostics->last_property,
                  "ro.build.version.sdk=36") != 0) {
    return fail("last Android property lookup was not retained");
  }

  ::dlclose(bionic);
  nuah_bootstrap_diagnostics_attach(nullptr);
  ::munmap(mapping, sizeof(*diagnostics));
  return 0;
}
