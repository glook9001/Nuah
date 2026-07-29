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

NuahJVM now retains the JNI runtime which Roblox accepted during `JNI_OnLoad`
and routes SDL input into its registered GameActivity callbacks. Android
framework façade coverage, surface lifecycle, and final game rendering remain
in progress. The transitional ATL fork is disabled by default
(`NUAH_BUILD_ATL=OFF`) and is not the target runtime.

## Build and run

```sh
cmake -S . -B build -G Ninja -DNUAH_BUILD_ATL=OFF
cmake --build build --target nuah nuah-services
./build/nuah config
./build/nuah native-run --apk /path/to/base.apk --split /path/to/split_config.x86_64.apk --uri 'roblox://placeId=1818'
```

`native-run` is the supervisor's Sober-style handoff. It uses NuahJVM, an
isolated adapter around the licensed android2gnulinux JNI core, loads the
x86_64 Roblox image through libhybris, and passes its JavaVM into
`JNI_OnLoad`. It does not yet provide Android framework classes or the final
lifecycle.
`NUAH_NATIVE_BIONIC_SMOKE=1` retains the separate API-36 Bionic-loader probe.
`atl-run` remains a legacy diagnostic command and is never selected by the
Services supervisor.

Nuah vendors Android 16's `libnativehelper` JNI headers as a pinned source
dependency. This supplies the official Android JNI ABI declarations only;
`libnativehelper`'s runtime invocation library delegates to ART and is not
used as an ART substitute.

The current native smoke path proves module loading, `JNI_OnLoad`, and
registration on one retained NuahJVM. It intentionally does not claim a
playable windowed game until the remaining Android object, surface lifecycle,
and rendering contracts are implemented.

## JNI contract workflow

Nuah must not discover Android Java methods one CI failure at a time.  The
versioned [`roblox-jni-contract.tsv`](share/roblox-jni-contract.tsv) is an
evidence ledger seeded from a working Sober capture.  CI parses and validates
it before any Roblox smoke test.  New façade work is admitted only after a
Sober dynamic trace or a static Roblox analysis records the exact
class/member/signature.  Entries with class `*` are deliberately unresolved
and cannot be counted as implemented façade coverage.

`nuah sober-cache-status` checks the Sober cache source. `nuah
adopt-sober-cache <directory>` copies that APK pair into Nuah-managed storage.
