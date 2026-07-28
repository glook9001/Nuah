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
- Sealed memfd loading primitives for APK native libraries.
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
./build/nuah atl-run --apk /path/to/base.apk --split /path/to/split_config.x86_64.apk --uri 'roblox://placeId=1818'
```

`atl-run` remains a transitional diagnostic command until the native
Sober-style game runtime is complete.

`nuah sober-cache-status` checks the Sober cache source. `nuah
adopt-sober-cache <directory>` copies that APK pair into Nuah-managed storage.
