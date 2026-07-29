#include "nuah/bootstrap_diagnostics.h"

#include <execinfo.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace {

NuahBootstrapDiagnostics* shared_diagnostics = nullptr;
volatile sig_atomic_t handling_fatal_signal = 0;

template <std::size_t Capacity>
void copy_text(char (&destination)[Capacity], const char* source) {
  if (!source) source = "";
  const std::size_t length = ::strnlen(source, Capacity - 1);
  std::memcpy(destination, source, length);
  destination[length] = '\0';
}

bool locate_mapping(uintptr_t caller, NuahBootstrapDiagnostics* diagnostics) {
  std::FILE* maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return false;
  bool found = false;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), maps)) {
    unsigned long long start = 0;
    unsigned long long end = 0;
    unsigned long long file_offset = 0;
    int path_offset = 0;
    if (std::sscanf(line, "%llx-%llx %*4s %llx %*s %*s %n", &start, &end,
                    &file_offset, &path_offset) < 3 ||
        caller < start || caller >= end) {
      continue;
    }
    diagnostics->module_offset =
        static_cast<uint64_t>(caller - start + file_offset);
    const char* path = line + path_offset;
    while (*path == ' ' || *path == '\t') ++path;
    const std::size_t length = std::strcspn(path, "\r\n");
    const std::size_t copied =
        length < sizeof(diagnostics->module_path) - 1
            ? length
            : sizeof(diagnostics->module_path) - 1;
    std::memcpy(diagnostics->module_path, path, copied);
    diagnostics->module_path[copied] = '\0';
    found = true;
    break;
  }
  std::fclose(maps);
  return found;
}

void record_address(uintptr_t caller) {
  if (!shared_diagnostics) return;
  shared_diagnostics->caller = caller;
  shared_diagnostics->thread_id =
      static_cast<int32_t>(::syscall(SYS_gettid));
  shared_diagnostics->module_path[0] = '\0';
  shared_diagnostics->module_offset = 0;
  locate_mapping(caller, shared_diagnostics);
  shared_diagnostics->abort_seen = 1;
}

void fatal_signal_handler(int signal_number, siginfo_t* info,
                          void* context_pointer) {
  if (!shared_diagnostics) return;
  shared_diagnostics->signal_number = signal_number;
  shared_diagnostics->fault_address =
      info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;

#if defined(__x86_64__) && defined(REG_RIP)
  if (context_pointer) {
    const auto* context = static_cast<const ucontext_t*>(context_pointer);
    record_address(
        static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_RIP]));
  }
#endif

  if (!std::strstr(shared_diagnostics->module_path, "/nuah-module-") &&
      !handling_fatal_signal) {
    handling_fatal_signal = 1;
    void* frames[64]{};
    const int count = ::backtrace(frames, 64);
    for (int index = 0; index < count; ++index) {
      record_address(reinterpret_cast<uintptr_t>(frames[index]));
      if (std::strstr(shared_diagnostics->module_path, "/nuah-module-")) break;
    }
  }

  if (std::strstr(shared_diagnostics->module_path, "/nuah-module-") &&
      !handling_fatal_signal) {
    handling_fatal_signal = 1;
    void* frames[64]{};
    const int count = ::backtrace(frames, 64);
    for (int index = 0; index < count; ++index) {
      NuahBootstrapDiagnostics frame{};
      if (locate_mapping(reinterpret_cast<uintptr_t>(frames[index]), &frame) &&
          std::strstr(frame.module_path, "/nuah-module-") &&
          frame.module_offset != shared_diagnostics->module_offset) {
        shared_diagnostics->parent_module_offset = frame.module_offset;
        break;
      }
    }
  }

  struct sigaction action {};
  action.sa_handler = SIG_DFL;
  ::sigemptyset(&action.sa_mask);
  (void)::sigaction(signal_number, &action, nullptr);
  sigset_t unblocked;
  ::sigemptyset(&unblocked);
  ::sigaddset(&unblocked, signal_number);
  (void)::sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
  (void)::syscall(SYS_tgkill, ::getpid(), ::syscall(SYS_gettid),
                  signal_number);
  ::_exit(128 + signal_number);
}

}  // namespace

extern "C" {

void nuah_bootstrap_diagnostics_attach(
    NuahBootstrapDiagnostics* diagnostics) {
  shared_diagnostics = diagnostics;
}

void nuah_bootstrap_diagnostics_set_stage(const char* stage) {
  if (!shared_diagnostics) return;
  copy_text(shared_diagnostics->stage, stage);
}

void nuah_bootstrap_diagnostics_record_abort(void* caller_address) {
  record_address(reinterpret_cast<uintptr_t>(caller_address));
}

void nuah_bootstrap_diagnostics_record_abort_message(const char* message) {
  if (!shared_diagnostics) return;
  copy_text(shared_diagnostics->abort_message, message);
}

void nuah_bootstrap_diagnostics_record_log(const char* message) {
  if (!shared_diagnostics) return;
  copy_text(shared_diagnostics->last_log, message);
}

void nuah_bootstrap_diagnostics_record_property(const char* key,
                                                const char* value) {
  if (!shared_diagnostics) return;
  std::snprintf(shared_diagnostics->last_property,
                sizeof(shared_diagnostics->last_property), "%s=%s",
                key ? key : "", value ? value : "");
}

NuahDiagnosticsCallbacks nuah_bootstrap_diagnostics_callbacks(void) {
  return NuahDiagnosticsCallbacks{
      1,
      nuah_bootstrap_diagnostics_record_abort,
      nuah_bootstrap_diagnostics_record_abort_message,
      nuah_bootstrap_diagnostics_record_log,
      nuah_bootstrap_diagnostics_record_property,
  };
}

void nuah_bootstrap_diagnostics_install_signal_handler(void) {
  struct sigaction action {};
  action.sa_sigaction = fatal_signal_handler;
  action.sa_flags = SA_SIGINFO;
  ::sigemptyset(&action.sa_mask);
  for (const int signal_number :
       {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV}) {
    (void)::sigaction(signal_number, &action, nullptr);
  }
}

}  // extern "C"
