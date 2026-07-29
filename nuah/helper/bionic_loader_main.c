#define _GNU_SOURCE

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ucontext.h>

#include "jni_facade.h"

static void jni_fault_handler(int signal_number, siginfo_t* info, void* context) {
  const ucontext_t* state = (const ucontext_t*)context;
#if defined(__x86_64__)
  const unsigned long long stack = (unsigned long long)state->uc_mcontext.gregs[REG_RSP];
  const unsigned long long caller = stack ? *(const unsigned long long*)stack : 0;
  fprintf(stderr,
          "bionic JNI fault: signal=%d address=%p rip=%#llx caller=%#llx\n",
          signal_number, info ? info->si_addr : 0,
          (unsigned long long)state->uc_mcontext.gregs[REG_RIP], caller);
#else
  fprintf(stderr, "bionic JNI fault: signal=%d address=%p\n", signal_number,
          info ? info->si_addr : 0);
#endif
  _Exit(128 + signal_number);
}

static void install_jni_fault_handler(void) {
  struct sigaction action;
  action.sa_sigaction = jni_fault_handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &action, 0);
}

/*
 * This program is deliberately built by the Android NDK, not the host
 * compiler.  It is started by API-36 linker64 and therefore both it and the
 * Roblox image use the extracted Android libc rather than Nuah callbacks.
 */
int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: nuah-bionic-loader <libroblox.so>\n");
    return 64;
  }
  void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!module) {
    const char* error = dlerror();
    fprintf(stderr, "bionic dlopen failed: %s\n", error ? error : "unknown error");
    return 1;
  }
  fprintf(stderr, "bionic dlopen succeeded: %s\n", argv[1]);
  install_jni_fault_handler();
  const int jni_status = nuah_jni_invoke_onload(module);
  if (jni_status != 0) return jni_status;
  return dlclose(module) == 0 ? 0 : 2;
}
