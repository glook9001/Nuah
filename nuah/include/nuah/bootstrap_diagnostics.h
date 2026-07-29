#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NUAH_BOOTSTRAP_TEXT_CAPACITY = 1024 };

typedef struct NuahBootstrapDiagnostics {
  uint32_t version;
  uint32_t abort_seen;
  int32_t signal_number;
  int32_t thread_id;
  uintptr_t caller;
  uintptr_t fault_address;
  uint64_t module_offset;
  uint64_t parent_module_offset;
  char stage[64];
  char module_path[512];
  char abort_message[NUAH_BOOTSTRAP_TEXT_CAPACITY];
  char last_log[NUAH_BOOTSTRAP_TEXT_CAPACITY];
  char last_property[256];
} NuahBootstrapDiagnostics;

typedef struct NuahDiagnosticsCallbacks {
  uint32_t version;
  void (*record_abort)(void* caller);
  void (*record_abort_message)(const char* message);
  void (*record_log)(const char* message);
  void (*record_property)(const char* key, const char* value);
} NuahDiagnosticsCallbacks;

void nuah_bootstrap_diagnostics_attach(NuahBootstrapDiagnostics* diagnostics);
void nuah_bootstrap_diagnostics_set_stage(const char* stage);
void nuah_bootstrap_diagnostics_record_abort(void* caller);
void nuah_bootstrap_diagnostics_record_abort_message(const char* message);
void nuah_bootstrap_diagnostics_record_log(const char* message);
void nuah_bootstrap_diagnostics_record_property(const char* key,
                                                const char* value);
NuahDiagnosticsCallbacks nuah_bootstrap_diagnostics_callbacks(void);
void nuah_bootstrap_diagnostics_install_signal_handler(void);

#ifdef __cplusplus
}
#endif
