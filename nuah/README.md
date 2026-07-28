# Nuah

Nuah is a Linux desktop wrapper around Android Translation Layer (ATL) for the
Roblox Android APK. It provides a separate WebKit Services window for sign-in
and runs Roblox through ATL.

```text
Services/WebKit -> Roblox launch URI -> ATL -> Roblox Android client
```

## Current implementation

- GTK4/libadwaita Services UI with Sober-like compact preference groups,
  Sober APK-cache discovery, and a WebKit Roblox launch bridge.
- ATL runtime launch with base and architecture-split APKs.
- Desktop-oriented keyboard, mouse, scroll, and Wayland pointer-capture work
  in the ATL fork.

The input path is still under active work; desktop camera capture and keyboard
delivery are not yet equivalent to Sober.

## Build and run

```sh
cmake -S . -B build -G Ninja
cmake --build build --target nuah nuah-services
./build/nuah config
./build/nuah atl-run --apk /path/to/base.apk --split /path/to/split_config.x86_64.apk --uri 'roblox://placeId=1818'
```

`nuah sober-cache-status` checks the Sober cache source. `nuah
adopt-sober-cache <directory>` copies that APK pair into Nuah-managed storage.
