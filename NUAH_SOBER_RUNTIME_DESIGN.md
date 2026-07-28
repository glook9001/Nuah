# Nuah Sober-style runtime design

## Purpose

Nuah should host the x86_64 Roblox Android client as a native Linux process,
following the behavior observed in Sober. It must not boot Android or depend on
an Android container, VM, emulator, system image, or SurfaceFlinger.

This document distinguishes observed Sober behavior from Nuah implementation
choices. The source evidence is in [Investigation/README.md](Investigation/README.md),
the [JNI report](Investigation/jni_callback_dependency_report.md), and the
[memfd timeline](Investigation/ebpf_deeplink_timeline.md).

## Observed Sober model

```text
sober_services                         sober / Main
  GTK + WebKit shell       IPC            native game runtime
        │                 ───►                 │
        │                                      ├─ self-remapped runtime
        │                                      ├─ Android symbol namespace
        │                                      ├─ JNI/JavaVM tables
        │                                      ├─ APK x86_64 client loader
        │                                      └─ host graphics/input
```

The browser/services process is separate from the game process. The game
process opens the local APK split, materializes `libroblox.so` into a sealed
memfd, and resolves Android imports into Sober's in-memory runtime. No
pathname-backed Android system libraries were observed.

Sober provides a JNI 1.6 `JNIEnv`/`JavaVM`, registered lifecycle/surface/input
callbacks, Android properties/logging, native-window/looper APIs, and a
synthetic namespace for Android sonames. Its Android Vulkan surface entrypoint
translates `vkCreateAndroidSurfaceKHR` to host
`vkCreateWaylandSurfaceKHR`; ordinary GLES calls largely reach host GLES.

## Sober library map

The investigation observed these native dependencies in Sober. They describe
the host-side shape Nuah should reproduce; they are not a requirement to copy
Sober's proprietary binaries.

### Game/runtime process

```text
libloader.so       protected APK/native-image loader
libmimalloc.so     private allocator
libEGL.so.1        host EGL boundary
libGLESv2.so.2     host GLES dispatch
libvulkan.so.1     host Vulkan loader
libgstreamer-1.0   media pipeline
libgstapp-1.0      application media buffers
libgstvideo-1.0    video support
libcurl.so.4       networking
libcrypto.so.3     TLS/crypto
libxml2.so.16      XML/data handling
libfreetype.so.6   fonts
libfontconfig.so.1 font discovery
libsecret-1.so.0   credential integration
libdbus-1.so.3     desktop/service IPC
libglib/gobject    host utility/object runtime
libz.so.1          compression
libc/libm/libgcc   host C/C++ ABI
```

The process also uses host Wayland/X11, Mesa Vulkan/GLES, PulseAudio, and
DMA-BUF support supplied by the Flatpak runtime and host system.

### Browser/services process

```text
libwebkitgtk-6.0.so.4
libjavascriptcoregtk-6.0.so.1
libgtk-4.so.1
libadwaita-1.so.0
libsoup-3.0.so.0
libgio-2.0.so.0
libglib-2.0.so.0
libgobject-2.0.so.0
libc/libgcc/ld-linux
```

Nuah maps these to `nuah-services` for the browser shell. The game process
must not depend on WebKitGTK, while the services process must not own the game
Vulkan surface.

### Android-facing compatibility namespace

Sober's client imports Android-named libraries that were not present as normal
files in the process. Nuah provides the equivalent boundary with libhybris'
Android linker and a restricted per-game library directory:

```text
libandroid.so liblog.so libEGL.so libGLESv2.so libvulkan.so
libmediandk.so libOpenSLES.so libOpenMAXAL.so libc.so libdl.so libm.so
```

`libc.so`, `libdl.so`, and `libm.so` are supplied through the libhybris/bionic
loader boundary, not reimplemented by Nuah on top of glibc. The other names
are narrow Nuah providers, translated to host libraries where appropriate, or
explicitly reported unsupported. This is not an Android system image.

## Nuah process architecture

```text
nuah-services
  ├─ WebKit/GTK login and navigation
  ├─ Roblox session state
  ├─ place selection and launch URI handling
  └─ framed Unix IPC
             │
             ▼
nuah-supervisor (nuah)
  ├─ validates session/place/profile
  ├─ creates one per-game data directory
  ├─ starts one nuah-main process
  ├─ forwards lifecycle/status events
  └─ isolates browser failures from game failures
             │
             ▼
nuah-main
  ├─ APK split/native-image loader
  ├─ libhybris Android loader + bionic compatibility boundary
  ├─ restricted Android provider directory
  ├─ JNI 1.6 and Java-object compatibility runtime
  ├─ Android lifecycle/input callback surface
  ├─ EGL/GLES/Vulkan translation boundary
  └─ host Wayland/Vulkan/GLES/audio
```

`nuah-services` owns the browser. `nuah-main` owns the game window and game
runtime. The supervisor must never render the game or depend on WebKit's event
loop for game input.

## APK and native loading

Nuah extracts `lib/x86_64/libroblox.so` from the selected APK split to a
private temporary file, then passes that path to libhybris `android_dlopen`.
The loader must preserve these useful properties:

1. Open the selected split APK.
2. Locate and inflate the x86_64 native entry.
3. Validate ELF class, machine, program headers, and required sonames.
4. Load it through libhybris' Android linker plugin, never `dlmopen`.
5. Resolve bionic imports through the matching libhybris runtime and resolve
   narrow platform imports from Nuah's provider directory.
6. Keep the temporary-file path until the Android loader closes the module.

`libtrampoline.so` is treated as an optional Crashpad helper, not as the
Android compatibility runtime.

## Android compatibility runtime

Nuah has one explicit boundary, but it does **not** recreate Android's C
runtime. Libhybris owns Android ELF loading, bionic symbol handling, and its
linker plugin. Nuah owns only APIs which have to meet the Linux desktop.

Nuah's Android-facing compatibility contract targets API level 36. Platform
release names are descriptive only; exported properties, symbol availability,
and tests use the numeric API level as the authoritative target.

Initial soname surface:

```text
libandroid.so      assets, configuration, looper, native window, input
liblog.so          Android logging
libEGL.so          Android-facing EGL dispatch
libGLESv2.so       GLES ABI bridge/pass-through
libvulkan.so       Android Vulkan entrypoints
libmediandk.so     media symbols required by the client
libOpenSLES.so     audio interface IDs and calls
libOpenMAXAL.so    optional media compatibility
```

The libhybris bundle is built and versioned as one x86_64 unit: its common
library, linker plugin, and compatible bionic runtime libraries. It has one
restricted search path containing the game image and Nuah's providers. Nuah
never compiles or ships synthetic replacements for `libc.so`, `libdl.so`, or
`libm.so`.

Every Nuah-provider symbol must be classified as `implemented`, `translated`,
or `unsupported`. Unsupported calls fail loudly with the symbol name; they
must not silently return fake success values.

## JNI and Java surface

The runtime supplies a real JNI 1.6 table and a small object/class model for
the classes observed through `JNI_OnLoad` and `RegisterNatives`. The first
contract includes:

- `FindClass`, method/field lookup, `Call*Method`, strings and arrays;
- exception check/clear and local/global references;
- `RegisterNatives` and the GameActivity lifecycle callbacks;
- surface/window callbacks;
- key, touch, text-input, and software-keyboard callbacks;
- logging, properties, storage, and MessageBus/WebView callbacks as required.

The implementation is intentionally demand-driven. A missing class or method
is recorded with its caller and signature so the surface can grow from real
Roblox behavior instead of from a speculative Android framework clone.

## Input

```text
Wayland compositor
        ↓
SDL3/host event backend + xkbcommon
        ↓
Nuah physical-key/pointer translator
        ↓
Android-style KeyEvent/MotionEvent and native callbacks
        ↓
Roblox GameActivity/Input interfaces
```

The translator preserves physical scancodes, Android keycodes, down/up state,
repeat, modifiers, timestamps, mouse buttons, motion, wheel axes, focus, and
pointer coordinates. `WASD`, `1`–`9`, mouse buttons, motion, and scrolling are
mandatory tests. Nuah must not read `/dev/input` directly or create a virtual
Android keyboard.

## Graphics

```text
Android-facing EGL/Vulkan calls
        ↓
Nuah native-window and dispatch registry
        ↓
vkCreateWaylandSurfaceKHR / host EGL/GLES/Vulkan
        ↓
Wayland surface and host GPU driver
```

`vkCreateAndroidSurfaceKHR` is translated by replacing the Android surface
create-info with a `VkWaylandSurfaceCreateInfoKHR` backed by Nuah's native
window. Host Vulkan and GLES remain the actual driver interfaces; no virtual
GPU or Android compositor is present.

## IPC and lifecycle

The browser-to-supervisor channel uses length-delimited records. Launch data
contains the place ID, session handoff, APK split path, profile directory, and
requested window mode. Status records include `starting`, `surface-ready`,
`running`, `failed`, and `exited`.

The supervisor owns cancellation and restart policy. A game crash closes only
`nuah-main`; the WebKit shell remains alive and reports the failure. Only one
game process may own a profile at a time.

## Security and ownership

- Keep session credentials in the services/supervisor boundary.
- Pass only the minimum launch data to `nuah-main`.
- Use a private namespace and per-game directory.
- Validate APK and native-image hashes before mapping.
- Do not copy Sober binaries or protected runtime code into Nuah.
- Reproduce observed ABI contracts with clean-room Nuah code.

## Build and distribution

Normal CI builds Nuah plus the pinned x86_64 libhybris common loader and its
linker plugin. A no-dependency ELF smoke test must pass through
`android_dlopen` before Roblox is attempted. CI does not sync or compile AOSP
ART and does not require an ART/APEX bundle. The old `art_standalone` source
tree and ART workflow are intentionally removed.

The runtime artifact contains Nuah, the libhybris bundle, Nuah's narrow
Android providers, and configuration. The Roblox APK remains a separately
acquired client payload.

## Implementation milestones

1. Build and package the pinned x86_64 libhybris loader and linker plugin.
2. Prove `android_dlopen` with a no-dependency x86_64 ELF.
3. Add the matching bionic runtime and Nuah provider directory; load
   `libroblox.so` through `android_dlopen` in CI.
4. Implement JNI table/object contracts observed in the investigation.
5. Implement native window, Android Vulkan-surface translation, and swapchain.
6. Implement physical keyboard/mouse translation and GameActivity callbacks.
7. Reproduce session IPC and launch `placeId=1818` through the WebKit shell.
8. Add crash isolation, graphics smoke tests, and input tests.

## Acceptance criteria

Nuah is considered Sober-aligned when it can launch the x86_64 Roblox client in
one native Linux game process, create a host Wayland/Vulkan surface, deliver
working keyboard/mouse/scroll input, preserve the WebKit session, and recover
the browser after a game-process failure—without an Android container, VM, or
virtual display. Libhybris and its narrow loader/bionic bundle are permitted;
an Android framework or full Android system image is not.
