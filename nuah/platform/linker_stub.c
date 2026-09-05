/*
 * Dependency-only ELF for libhybris' Android linker.
 *
 * These files are deliberately not Android libc implementations.  Their only
 * job is to give upstream libhybris a soname record for DT_NEEDED entries;
 * libhybris' hook callback supplies the actual symbols from the host and from
 * Nuah's narrow providers.  Keeping the files dependency-free avoids loading
 * glibc-linked provider DSOs through the Android linker.
 */
void __system_properties_init(void) {}
