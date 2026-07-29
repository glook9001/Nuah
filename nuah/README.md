# Nuah

Nuah is a Linux desktop runtime for the Roblox Android APK, following the
native process and Android-ABI boundary observed in Sober. It provides a
separate WebKit Services window for sign-in and a supervised native game
process.

```text
Services/WebKit -> framed IPC -> supervisor -> native game runtime -> Roblox
```

## Current implementation

- GTK4/libadwaita Services UI with Sober-like compact preference groups,
  Sober APK-cache discovery, and a WebKit Roblox launch bridge.
- APK split discovery and x86_64 native-image validation.
- Isolated temporary-file loading primitives for APK native libraries.
- Native Vulkan Android-surface translation to host Wayland.
- An explicit Android ABI registry with fail-loud unsupported calls.

The Sober-style JNI/object runtime, Android input callback path, and final game
loader are still being implemented. The transitional ATL fork is disabled by
default (`NUAH_BUILD_ATL=OFF`) and is not the target runtime.

## Build and run

```sh
cmake -S . -B build -G Ninja -DNUAH_BUILD_ATL=OFF
cmake --build build --target nuah nuah-services
./build/nuah config
./build/nuah native-run --apk /path/to/base.apk --split /path/to/split_config.x86_64.apk --uri 'roblox://placeId=1818'
```

`native-run` is the supervisor's Sober-style handoff. It loads the host JDK's
`libjvm.so`, loads the x86_64 Roblox image through libhybris, and passes the
real OpenJDK `JavaVM` into `JNI_OnLoad`. Set `NUAH_JVM_LIBRARY` to an explicit
`libjvm.so` when the JDK is not in a normal system location. This proves the
JNI boundary without bundling ART, an Android system image, or a container.
It does not yet provide Android framework classes or the final lifecycle.
`NUAH_NATIVE_BIONIC_SMOKE=1` retains the separate API-36 Bionic-loader probe.
`atl-run` remains a legacy diagnostic command and is never selected by the
Services supervisor.

Nuah vendors Android 16's `libnativehelper` JNI headers as a pinned source
dependency. This supplies the official Android JNI ABI declarations only;
`libnativehelper`'s runtime invocation library delegates to ART and is not
used as an ART substitute.

For host-window/event-loop diagnostics, set `NUAH_NATIVE_WINDOW_LOOP=1`.
`NUAH_NATIVE_LIFECYCLE=1` additionally attempts the observed GameActivity
initialization callbacks; use `NUAH_NATIVE_MAX_FRAMES` to bound a diagnostic
session. The normal supervisor keeps these opt-in until the remaining Android
object and rendering contracts are implemented.

`nuah sober-cache-status` checks the Sober cache source. `nuah
adopt-sober-cache <directory>` copies that APK pair into Nuah-managed storage.
