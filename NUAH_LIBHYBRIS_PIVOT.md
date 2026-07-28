# Nuah libhybris loader pivot

## Decision

This branch replaces Nuah's experimental `dlmopen` plus synthetic
`libc.so`/`libdl.so`/`libm.so` compatibility path with an Android-loader
path.  The target remains one normal Linux process: this is **not** a
Waydroid-style Android container, VM, or system image.

```
Roblox x86_64 APK ELF
        |
Nuah APK extractor (temporary file during the pivot)
        |
libhybris android_dlopen
        |
libhybris host ABI hooks and Android linker plugin
        |
Nuah-owned libandroid/log/Vulkan/window/input/JNI services
        |
host Wayland + Mesa Vulkan + Linux input
```

`dlmopen` is not retained underneath libhybris.  It is a glibc namespace
loader and cannot provide Android linker semantics or load Android bionic
DSOs correctly.  The current direct implementation is preserved on `main`
as a diagnostic baseline, not deleted.

## Bundle boundary

The pivot bundle contains only host-side libhybris pieces which must agree on a
single ABI:

- libhybris common library and its x86_64 linker plugin;
- a generated linker namespace configuration containing only Nuah's Android
  library directory and the game image directory.

Nuah continues to provide `libandroid.so`, `liblog.so`, `libvulkan.so`,
`libmediandk.so`, `libOpenSLES.so`, and `libOpenMAXAL.so`, forwarding the
host-facing pieces to its Wayland/Vulkan implementation.  It also continues
to own JNI, GameActivity lifecycle, input, the WebKit service, and crash
supervision.  Libhybris does not implement any of those.

The bundle must be sourced and versioned as one unit.  Nuah never imports an
Android `libc.so`, `libdl.so`, `libm.so`, APEX, or system image; libhybris'
hooks bridge those Android imports to the host ABI.

The pinned libhybris source carries one small Nuah patch. Its Q-era linker
models the three bionic names above as in-memory, hook-only dependency records
when they appear in `DT_NEEDED`. This is necessary because upstream otherwise
requires files for every dependency before it reaches its hook resolver. The
records contain no ELF image, code, symbols, bionic library, or Android
payload; normal Android-facing libraries remain real, narrow Nuah providers.

## Implementation order

1. Pin libhybris and exercise its x86_64 linker route with a trivial DSO.
   Upstream documentation warns that 64-bit support is incomplete and the
   pinned plugin is Android-Q-era, not API 36. Nuah must establish API-36
   compatibility in the host-side hooks/linker path before claiming support;
   it must not silently fall back to `dlmopen` or import Android runtime
   files.
2. Add `HybrisLoader` beside the existing `ApkLoader`.  It calls
   `android_dlopen`/`android_dlsym`; it does not call `dlmopen`.
3. Generate the restricted Android linker namespace at launch and expose
   only the versioned bundle plus Nuah's Android provider directory.
4. Retire the synthetic bionic, dl, and math providers from the Nuah build.
   Libhybris is the sole Android loader/bionic boundary.
5. Drive the real game through `JNI_OnLoad`, GameActivity lifecycle, host
   window/input, and Vulkan.  Keyboard correctness is verified by WASD,
   `1`–`9`, mouse buttons/motion, and wheel events in a running room.

## Non-goals

- No ART, Dalvik, Android framework, SurfaceFlinger, Android container, or
  Android VM is bundled.
- No local compilation.  Any source build or bundle-validation build runs in
  GitHub Actions; the released Nuah artifact consumes a pinned bundle.
- No claim that libhybris fixes input by itself.  It removes the wrong loader
  boundary; Nuah's SDL/XKB-to-Android native input bridge remains responsible
  for native keyboard and mouse events.

## Gates before making this the default backend

1. x86_64 libhybris linker-plugin smoke test succeeds with a representative
   API-36-targeted DSO using host hooks only (no bionic/APEX payload).
2. `libroblox.so` loads without Nuah's synthetic libc/libdl/libm providers.
3. `JNI_OnLoad` executes and reports concrete missing JNI contracts, rather
   than a loader error.
4. The existing native window receives keyboard, mouse, and wheel input.

Until gates 1–2 pass, the pivot is explicitly experimental and cannot be
advertised as a launchable Roblox runtime.
