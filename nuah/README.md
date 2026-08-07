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

Nuah has never produced a playable Roblox session. The native path is an
experimental Android-facing façade: it can exercise parts of loading and JNI
registration, but it must not be described as a working or legacy runtime.
The ATL build is disabled by default (`NUAH_BUILD_ATL=OFF`) and is not the
target runtime.

## Build and run

```sh
cmake -S . -B build -G Ninja -DNUAH_BUILD_ATL=OFF
cmake --build build --target nuah nuah-services
./build/nuah config
./build/nuah native-run --apk /path/to/base.apk --split /path/to/split_config.x86_64.apk --uri 'roblox://placeId=1818'
```

`native-run` is the only supported Nuah game path. It uses NuahJVM, an
isolated adapter around the licensed android2gnulinux JNI core, loads the
x86_64 Roblox image through libhybris, and prepares the JavaVM passed to
`JNI_OnLoad`. The default `NUAH_FAST_MVP=1` route attempts the smallest direct
NativeGLInterface bootstrap needed to reach a first frame; it is still
experimental and currently does not claim a playable result. Set
`NUAH_FAST_MVP=0` only to compare the older parameter-setter ABI while
diagnosing the boundary; it is not an alternate working runtime.
`NUAH_NATIVE_BIONIC_SMOKE=1` retains the separate API-36 Bionic-loader probe.
`atl-run` is outside the Nuah native MVP and is never selected by the Services
supervisor.

Nuah vendors Android 16's `libnativehelper` JNI headers as a pinned source
dependency. This supplies the official Android JNI ABI declarations only;
`libnativehelper`'s runtime invocation library delegates to ART and is not
used as an ART substitute.

The current native smoke path proves only portions of loading, relocation, and
JNI registration through the libhybris Android linker, including host-backed
DT_NEEDED libraries. A visible window or a successful `JNI_OnLoad` smoke is
not a playable game; the remaining Android-native bootstrap, object, surface,
lifecycle, and rendering contracts must still be proven.

## Documentation-driven Android façade

Nuah implements the smallest subset of Android contracts actually observed at
the Roblox boundary. The official [GameActivity documentation](https://developer.android.com/games/agdk/game-activity/get-started)
defines the normal lifecycle, `android_native_app_glue`, `ALooper`, and
input-buffer model; the [NDK native-window documentation](https://developer.android.com/ndk/reference/group/native-activity)
defines `ANativeWindow` ownership and lifetime; the [JNI guidance](https://developer.android.com/ndk/guides/jni-tips)
defines the one-`JavaVM`/thread-local-`JNIEnv` rule. These are interface
contracts, not a requirement to build ART or the Android framework. The local
[android2gnulinux README](../third_party/android2gnulinux/README.md)
explicitly describes its linker/JVM as incomplete and its initial JNI calls as
manual, so it is used only as a narrow JNI core rather than a complete Android
runtime.

The fast MVP deliberately defers the complete GameActivity queue and optional
Android services. It keeps one libhybris-loaded Roblox image, one Nuah JVM,
one SDL/Vulkan window, one Surface façade, extracted asset files, and the
minimal keyboard/mouse callbacks required for a first playable test. Each
new façade method must be backed by a documented ABI or an observed Roblox
trace and must have a focused local smoke test before it is admitted.

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
