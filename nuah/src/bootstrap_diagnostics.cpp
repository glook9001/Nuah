#include "nuah/bootstrap_diagnostics.h"

#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
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
    auto* context = static_cast<ucontext_t*>(context_pointer);
    /* A current Roblox GLES build can enter its render-target allocator with
     * an uninitialised aspect vector.  The resulting NaN is converted to a
     * zero divisor at one stable instruction.  Keep this recovery explicitly
     * opt-in: it lets us verify the rest of the frame path without turning a
     * real native fault into an invisible hang. */
    if (signal_number == SIGFPE &&
        std::getenv("NUAH_RECOVER_RENDER_FPE")) {
      NuahBootstrapDiagnostics mapping{};
      const uintptr_t pc = static_cast<uintptr_t>(
          context->uc_mcontext.gregs[REG_RIP]);
      const auto* instruction = reinterpret_cast<const unsigned char*>(pc);
      const auto* preceding = instruction - 2;
      /* The offset moves with every Roblox APK.  The fault itself is stable:
       * `xor edx, edx; div ecx`, with ecx zero and the pixel extent in r14d.
       * Match those bytes and the register precondition instead of pinning a
       * build-specific libroblox offset. */
      if (locate_mapping(pc, &mapping) &&
          std::strstr(mapping.module_path, "/libroblox.so") &&
          preceding[0] == 0x31 && preceding[1] == 0xd2 &&
          instruction[0] == 0xf7 && instruction[1] == 0xf1 &&
          context->uc_mcontext.gregs[REG_RCX] == 0 &&
          context->uc_mcontext.gregs[REG_R14] != 0) {
        constexpr uint64_t kFallbackDivisor = 512;
        context->uc_mcontext.gregs[REG_RAX] =
            context->uc_mcontext.gregs[REG_R14] / kFallbackDivisor;
        context->uc_mcontext.gregs[REG_RDX] = 0;
        context->uc_mcontext.gregs[REG_RCX] = kFallbackDivisor;
        context->uc_mcontext.gregs[REG_RIP] =
            static_cast<greg_t>(pc + 2);  // skip `div ecx`
        static constexpr char message[] =
            "nuah: recovered Roblox NaN render divisor (diagnostic)\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        return;
      }
    }
    unsigned int xmm0_bits = 0;
    unsigned int xmm1_bits = 0;
    unsigned int xmm2_bits = 0;
    unsigned int xmm3_bits = 0;
    if (context->uc_mcontext.fpregs) {
      std::memcpy(&xmm0_bits, &context->uc_mcontext.fpregs->_xmm[0],
                  sizeof(xmm0_bits));
      std::memcpy(&xmm1_bits, &context->uc_mcontext.fpregs->_xmm[1],
                  sizeof(xmm1_bits));
      std::memcpy(&xmm2_bits, &context->uc_mcontext.fpregs->_xmm[2],
                  sizeof(xmm2_bits));
      std::memcpy(&xmm3_bits, &context->uc_mcontext.fpregs->_xmm[3],
                  sizeof(xmm3_bits));
    }
    char registers[256]{};
    const int register_length = std::snprintf(
        registers, sizeof(registers),
        "nuah crash registers: rip=%#llx rdi=%#llx rbx=%#llx r12=%#llx "
        "r13=%#llx r14=%#llx r15=%#llx rax=%#llx rcx=%#llx "
        "xmm0=%#x xmm1=%#x xmm2=%#x xmm3=%#x\\n",
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_RIP]),
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_RDI]),
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_RBX]),
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_R12]),
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_R13]),
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_R14]),
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_R15]),
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_RAX]),
        static_cast<unsigned long long>(context->uc_mcontext.gregs[REG_RCX]),
        xmm0_bits, xmm1_bits, xmm2_bits, xmm3_bits);
    if (register_length > 0)
      (void)::write(STDERR_FILENO, registers,
                    static_cast<std::size_t>(register_length));
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
