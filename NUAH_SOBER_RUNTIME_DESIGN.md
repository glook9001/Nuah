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
  ├─ Android soname/symbol registry
  ├─ JNI 1.6 and Java-object compatibility runtime
  ├─ Android lifecycle/input callback surface
  ├─ EGL/GLES/Vulkan translation boundary
  └─ host Wayland/Vulkan/GLES/audio
```

`nuah-services` owns the browser. `nuah-main` owns the game window and game
runtime. The supervisor must never render the game or depend on WebKit's event
loop for game input.

## APK and native loading

The first implementation may load `lib/x86_64/libroblox.so` from the APK using
a private temporary file. The production loader should match Sober's useful
properties:

1. Open the selected split APK.
2. Locate and inflate the x86_64 native entry.
3. Validate ELF class, machine, program headers, and required sonames.
4. Map the image in a private loader namespace.
5. Resolve Android imports through Nuah's registry.
6. Seal the backing memfd after population when the platform supports it.

`libtrampoline.so` is treated as an optional Crashpad helper, not as the
Android compatibility runtime.

## Android compatibility runtime

Nuah should implement one explicit Android ABI registry rather than many
unrelated placeholder libraries.

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
libc.so/libdl.so   narrow Android ABI/linker compatibility
```

Every exported symbol must be classified as `implemented`, `translated`,
`forwarded`, or `unsupported`. Unsupported calls fail loudly with the symbol
name; they must not silently return fake success values.

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

Normal CI builds Nuah's native runtime and ATL support only. It does not sync
or compile AOSP ART and does not require an ART/APEX bundle. The old
`art_standalone` source tree and ART workflow are intentionally removed.

The runtime artifact contains Nuah/ATL binaries, the Android ABI registry, and
configuration. The Roblox APK remains a separately acquired client payload.

## Implementation milestones

1. Replace placeholder Android stubs with the registry and symbol diagnostics.
2. Implement APK x86_64 loading and a disk-backed development path.
3. Implement JNI table/object contracts observed in the investigation.
4. Implement native window, Android Vulkan-surface translation, and swapchain.
5. Implement physical keyboard/mouse translation and GameActivity callbacks.
6. Reproduce session IPC and launch `placeId=1818` through the WebKit shell.
7. Add crash isolation, ABI tests, graphics smoke tests, and input tests.
8. Add sealed memfd loading after the ordinary loader path is stable.

## Acceptance criteria

Nuah is considered Sober-aligned when it can launch the x86_64 Roblox client in
one native Linux game process, create a host Wayland/Vulkan surface, deliver
working keyboard/mouse/scroll input, preserve the WebKit session, and recover
the browser after a game-process failure—without Android APEX files, AOSP
source, a container, a VM, or a virtual display.
