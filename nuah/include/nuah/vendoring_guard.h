#ifndef NUAH_VENDERING_GUARD_H
#define NUAH_VENDERING_GUARD_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Pure predicate, unit-tested: nonzero when path is a banned system location.
 * Only /usr/local is banned: the host libc, Mesa, and distro libraries under
 * /usr and /lib64 stay allowed. */
int nuah_path_is_banned_system_path(const char* path);
/* Scans the host (glibc) link map; returns the banned entry count and copies
 * the first offender (empty string when clean). Hybris-namespace libraries
 * are invisible to the host linker by design and stay covered by hermetic
 * load-path defaults instead. */
int nuah_vendoring_guard_scan(char* offender, size_t offender_len);
/* Loud enforcement: aborts the process when banned entries are loaded. */
void nuah_require_vendored_link_map(void);
#ifdef __cplusplus
}
#endif
#endif
