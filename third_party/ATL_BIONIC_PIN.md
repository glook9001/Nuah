# ATL Bionic translation dependency

Nuah vendors `android_translation_layer/bionic_translation` at:

```text
ee37eb21c91409fe0eed833d0a5a0aa6b931bb7b
```

Upstream: <https://gitlab.com/android_translation_layer/bionic_translation>

Nuah uses `libdl_bio` as its Android multi-image translation linker and plans
to use `libc_bio`/`libpthread_bio` as the Bionic ABI providers. The project is
built in `build/atl-bionic` through its upstream Meson build.

Do not replace this with full Android Translation Layer/ART: Nuah supplies its
own narrow JNI object runtime and uses SDL3 rather than ATL's GTK-owned activity
window.

Local build-only adjustment: verbose per-symbol linker tracing is disabled in
the vendored Meson file. Errors and missing-symbol diagnostics remain enabled;
this prevents a single Roblox relocation pass from producing millions of log
lines. The upstream unconditional per-APS2-relocation trace is disabled for the
same reason.

Nuah also batches the decoded APS2 entries into one `apkenv_reloc_library`
call. Upstream called the general relocation engine once per entry, which took
longer than 60 seconds for Roblox.
