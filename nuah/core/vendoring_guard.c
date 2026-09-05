#include "nuah/vendoring_guard.h"
#include <link.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int nuah_path_is_banned_system_path(const char* path) {
  static const char kPrefix[] = "/usr/local/";
  if (!path || !*path) return 0;
  if (strncmp(path, kPrefix, sizeof(kPrefix) - 1) == 0) return 1;
  return strcmp(path, "/usr/local") == 0;
}
struct NuahGuardScanState { char* offender; size_t offender_len; int count; };
static int nuah_guard_visit(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  struct NuahGuardScanState* state = (struct NuahGuardScanState*)data;
  if (info->dlpi_name && info->dlpi_name[0] && nuah_path_is_banned_system_path(info->dlpi_name)) {
    if (state->count == 0 && state->offender && state->offender_len > 0) {
      snprintf(state->offender, state->offender_len, "%s", info->dlpi_name);
    }
    state->count++;
  }
  return 0;
}
int nuah_vendoring_guard_scan(char* offender, size_t offender_len) {
  struct NuahGuardScanState state = {offender, offender_len, 0};
  if (offender && offender_len > 0) offender[0] = '\0';
  dl_iterate_phdr(nuah_guard_visit, &state);
  return state.count;
}
void nuah_require_vendored_link_map(void) {
  char offender[1024];
  if (nuah_vendoring_guard_scan(offender, sizeof(offender)) == 0) return;
  fprintf(stderr, "nuah: banned system library loaded: %s (/usr/local is forbidden; vendoring required)\n", offender);
  abort();
}
