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
API-36 Android linker + matching bionic libc/libdl/libm
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

The pivot bundle contains only loader/runtime pieces which must agree on a
single Android ABI:

- libhybris common library and its x86_64 linker plugin;
- Android API-36 linker support plus matching `libc.so`, `libdl.so`, and
  `libm.so`;
- a generated linker namespace configuration containing only Nuah's Android
  library directory and the game image directory.

Nuah continues to provide `libandroid.so`, `liblog.so`, `libvulkan.so`,
`libmediandk.so`, `libOpenSLES.so`, and `libOpenMAXAL.so`, forwarding the
host-facing pieces to its Wayland/Vulkan implementation.  It also continues
to own JNI, GameActivity lifecycle, input, the WebKit service, and crash
supervision.  Libhybris does not implement any of those.

The bundle must be sourced and versioned as one unit.  Mixing a current
`libc.so` with an older libhybris linker plugin is rejected: it is an ABI
boundary, not an interchangeable collection of `.so` files.

## Implementation order

1. Pin libhybris and exercise its x86_64 linker route with a trivial Android
   API-36 DSO.  Upstream documentation warns that 64-bit support is
   incomplete; Nuah accepts that as an experimental compatibility risk and
   records it in CI rather than silently falling back to `dlmopen`.
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

1. x86_64 libhybris linker-plugin smoke test succeeds with an API-36 bionic
   DSO.
2. `libroblox.so` loads without Nuah's synthetic libc/libdl/libm providers.
3. `JNI_OnLoad` executes and reports concrete missing JNI contracts, rather
   than a loader error.
4. The existing native window receives keyboard, mouse, and wheel input.

Until gates 1–2 pass, the pivot is explicitly experimental and cannot be
advertised as a launchable Roblox runtime.
