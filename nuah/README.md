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

The current local MVP reaches a rendered Roblox place and accepts the
keyboard/mouse path: W/A/S/D movement, arrow-key camera input, number keys,
relative mouse movement, buttons, and wheel events. The ATL build remains
disabled by default (`NUAH_BUILD_ATL=OFF`); the playable path uses the
libhybris loader with the installed API-36 ART provider.

## Build and run

```sh
cmake -S . -B build -G Ninja -DNUAH_BUILD_ATL=OFF
cmake --build build --target nuah nuah-services
./build/nuah config
```

`native-run` is the supported Nuah game path. From the repository root, this
is the known-working local launch for RIVALS (place `17625359962`) with the
current Sober APK and login cookie. Run Sober first and sign in so its cookie
store and APK package are current. Adjust the provider/APK paths if they are
installed elsewhere:

```sh
SOBER_DATA="$HOME/.var/app/org.vinegarhq.Sober/data/sober"
SOBER_PACKAGE="$SOBER_DATA/packages/x86_64/com.roblox.client"
ROBLOX_COOKIE="$(sed -n 's/.*\.ROBLOSECURITY=\([^;[:space:]]*\).*/\1/p' \
  "$SOBER_DATA/cookies" | head -1)"
export NUAH_ROBLOX_COOKIES=".ROBLOSECURITY=$ROBLOX_COOKIE"
export NUAH_ROBLOX_COOKIE_HEADER=".ROBLOSECURITY=$ROBLOX_COOKIE"
export NUAH_ATL_NATIVE_DIR="$HOME/.local/share/nuah/base.apk_/lib"
export NUAH_HYBRIS_LIBRARY="$HOME/.local/share/nuah/hybris/lib/libhybris-common.so"
export HYBRIS_LINKER_DIR="$HOME/.local/share/nuah/hybris/lib/libhybris/linker"
export LD_LIBRARY_PATH=/usr/local/lib64/art:"$HOME/.local/share/nuah/hybris/lib"
export LD_PRELOAD=/usr/lib64/libpng16.so.16:/usr/lib64/libjpeg.so.62:/usr/local/lib64/art/libandroidfw.so
export NUAH_GRAPHICS_BACKEND=vulkan
export NUAH_PERFORMANCE_MODE=turbo
export NUAH_VULKAN_PRESENT_MODE=fifo
export NUAH_VULKAN_SUBMIT_THREAD=1
export MESA_VK_ENABLE_SUBMIT_THREAD=1
export NUAH_INPUT_COALESCE=1
export NUAH_NONBLOCK_WAYLAND_EVENTS=0
export NUAH_MOUSE_CAPTURE=1
export NUAH_FAST_RENDER=1
export NUAH_ASSET_BACKGROUND=1
# Keep Roblox's normal transport. Setting this to 1 causes RIVALS error 257.
export NUAH_DISABLE_RBX_TRANSPORT_DUMMY=0
export NUAH_TASK_THREADS=4
export NUAH_TARGET_FPS=60
export NUAH_SHADER_CACHE_DIR="$HOME/.local/share/nuah/base.apk_/mesa-shader-cache"

./build/nuah native-run \
  --apk "$SOBER_PACKAGE/base.apk" \
  --split "$SOBER_PACKAGE/split_config.x86_64.apk" \
  --data "$HOME/.local/share/nuah" \
  --uri 'roblox://placeId=17625359962'
```

Do not print or commit `ROBLOX_COOKIE`; it is an authentication credential.
If the cookie is empty, sign in through Sober again before launching Nuah.

The launch flags above are the tested play profile. `NUAH_GRAPHICS_BACKEND`,
`NUAH_PERFORMANCE_MODE`, `NUAH_VULKAN_PRESENT_MODE`,
`NUAH_VULKAN_SUBMIT_THREAD`, `MESA_VK_ENABLE_SUBMIT_THREAD`,
`NUAH_INPUT_COALESCE`, `NUAH_NONBLOCK_WAYLAND_EVENTS`, `NUAH_MOUSE_CAPTURE`,
`NUAH_FAST_RENDER`, `NUAH_ASSET_BACKGROUND`, `NUAH_TASK_THREADS`,
`NUAH_TARGET_FPS`, and `NUAH_SHADER_CACHE_DIR` control rendering, scheduling,
input, and caching. `NUAH_ATL_NATIVE_DIR`, `NUAH_HYBRIS_LIBRARY`,
`HYBRIS_LINKER_DIR`, `LD_LIBRARY_PATH`, and `LD_PRELOAD` select the Android
runtime boundary. `NUAH_ROBLOX_COOKIES` and `NUAH_ROBLOX_COOKIE_HEADER` pass
the Sober session to Roblox. `NUAH_DISABLE_RBX_TRANSPORT_DUMMY=0` is
intentional: the dummy-transport disable override is not compatible with the
current RIVALS server handshake.

An optional host-owned loading frame can cover Roblox's cold scene transition;
enable it explicitly with `NUAH_LOADING_FRAME=1`. It is held for 10 seconds
and has a 30-second safety timeout (`NUAH_LOADING_FRAME_MIN_MS` and
`NUAH_LOADING_FRAME_TIMEOUT_MS` adjust those values). It is disabled during
normal gameplay so it cannot affect frame-pacing evaluation.
Before the join, Nuah also issues bounded `posix_fadvise(WILLNEED)` hints for
already-extracted local assets; remote downloads and Android decoding remain
Roblox-owned. Set `NUAH_ASSET_PREFETCH=0` or adjust the bound with
`NUAH_ASSET_PREFETCH_MB` (512 MiB by default).

This uses NuahJVM, an isolated adapter around the licensed android2gnulinux
JNI core, loads the x86_64 Roblox image through libhybris, and prepares the
JavaVM passed to `JNI_OnLoad`. `NUAH_FAST_MVP=1` is the default direct
NativeGLInterface bootstrap that reaches the first frame. Set
`NUAH_FAST_MVP=0` only to compare the older parameter-setter ABI while
diagnosing the boundary; it is not the normal launch path.
`NUAH_NATIVE_BIONIC_SMOKE=1` retains the separate API-36 Bionic-loader probe.
`atl-run` is outside the Nuah native MVP and is never selected by the Services
supervisor.

Nuah vendors Android 16's `libnativehelper` JNI headers as a pinned source
dependency. This supplies the official Android JNI ABI declarations only;
`libnativehelper`'s runtime invocation library delegates to ART and is not
used as an ART substitute.

The current native path exercises loading, relocation, JNI registration,
Android lifecycle/surface setup, rendering, authenticated place joining, and
the Sober-style keyboard/mouse callbacks through the libhybris Android linker.

The loader reuses `--data`'s extracted `lib/libroblox.so` when its size matches
the APK member, instead of copying the roughly 116-MiB image into a new
temporary file on every launch.  The `.native-tmp/nuah-module-*` directory is
only a fallback for callers without an app-private extraction root; stale
fallback files are reaped at the next launch because the isolated child exits
without running C++ destructors.

For the guarded texture-generator experiment, create a sidecar from the exact
APK image with `nuah/tools/patch_libroblox_texture.py`, then opt into it for a
single run:

```sh
python3 nuah/tools/patch_libroblox_texture.py \
  --input "$HOME/.local/share/nuah/base.apk_/lib/libroblox.so" \
  --output "$HOME/.local/share/nuah/base.apk_/lib/libroblox.patched.so" \
  --manifest "$HOME/.local/share/nuah/base.apk_/lib/libroblox.patched.so.json"
NUAH_LIBROBLOX_PATCH="$HOME/.local/share/nuah/base.apk_/lib/libroblox.patched.so" \
  ./build/nuah native-run ...
```

The patcher is build-ID and SHA-256 locked, changes one two-byte default flag
write (`TexturePackGeneratorUseOriginal`), and never overwrites the APK image.
Nuah validates the sidecar manifest, hashes, ELF build ID, and replacement bytes
before loading it.  Unset `NUAH_LIBROBLOX_PATCH` to return to the unmodified
image immediately.

For a no-copy A/B, apply the same two-byte change to the mapped image after
`dlopen` and before ART can call `JNI_OnLoad`:

```sh
NUAH_LIBROBLOX_MEMORY_PATCH=1 ./build/nuah native-run ...
# `NUAH_LIBROBLOX_PATCH=memory` is an equivalent spelling.
```

The in-memory path does not hash or rewrite the APK.  It looks for the
`TexturePackGeneratorUseOriginal` string together with its unique x86-64
constructor sequence, derives the loaded address from the image's exported
symbol, temporarily enables write access on that one code page, flushes the
instruction cache, and restores the original permissions.  It refuses a
missing or ambiguous sequence and reports the failure before JNI startup.

The local build also places its pinned bionic-translation pthread provider in
`build/bionic-translation/`. `native-run` selects its pointer-tagged
mutex/condition wrapper automatically when present. This keeps the API-36
objects compatible while avoiding the compatibility table's global lookup
spin lock on Roblox worker paths. Set `NUAH_PTHREAD_SYNC=table` to restore the
older table adapter for comparison; no system library replacement or `sudo`
install is required.

The same provider now owns Android rwlocks by default; set
`NUAH_PTHREAD_RWLOCK=0` to compare the old host-libc rwlock path. The bridge
uses the client’s API-36 object layout while keeping the process in one host
TLS domain.
The bridge defaults to writer-preferred host rwlocks to avoid reader starvation
in Roblox's data-model workers; set `NUAH_PTHREAD_RWLOCK_POLICY=default` to
restore glibc's policy for comparison.
Early binding during the initial `RTLD_NOW` relocation is now the default;
set `NUAH_ANDROID_SYNC_EARLY=0` for the old host-libc/constructor comparison.
The low-end desktop profile also removes Roblox's deliberate 150 ms asset
workflow sleep and uses up to three scheduler/two asset workers; explicit
`NUAH_TASK_THREADS`, `NUAH_ASSET_PROVIDER_THREADS`, or
`NUAH_ASSET_WORKFLOW_SLEEP_US` values still override it.

For a bounded synchronization diagnostic, set `NUAH_PTHREAD_TRACE=1`. The
bridge prints a summary containing `mincore` probes, tagged versus fallback
objects, mapping failures, and condition/semaphore/rwlock wait time. Because
the native child normally uses `_exit`, send `SIGUSR1` to that child for a live
snapshot (`kill -USR1 <native-pid>`); an ordinary `exit` also prints it. Leave
the variable unset for normal gameplay; it adds no clock reads or output to
the hot path.

When no client-settings response is supplied, the native launcher now selects
Vulkan by default. Set `NUAH_GRAPHICS_BACKEND=opengl` (or `gles`) for the
explicit OpenGL fallback; an explicit `NUAH_CLIENT_SETTINGS_JSON` or
`NUAH_CLIENT_SETTINGS_PATH` always takes precedence.

## Vulkan performance checks

Vulkan is the normal renderer.  Nuah defaults to the driver's FIFO mode, which
matches Android's compositor-paced swap loop and gives steadier frame timing
on Wayland/Intel.  For an A/B run, set one of these before starting
`native-run`:

```sh
NUAH_VULKAN_PRESENT_MODE=fifo      # compositor-paced (the default)
NUAH_VULKAN_PRESENT_MODE=fifo_relaxed # recovers faster after a missed refresh
NUAH_VULKAN_PRESENT_MODE=immediate # lowest queue latency, may tear/stutter
NUAH_VULKAN_PRESENT_MODE=mailbox   # low latency when the driver advertises it
```

Turbo Vulkan also negotiates the four-image FIFO swapchain used by Sober. The
extra queued image keeps the compositor fed while Roblox streams a scene and
does not change the Android surface ABI. Set `NUAH_VULKAN_MIN_IMAGE_COUNT=3`
to compare the previous three-image queue, or set another value when the host
driver advertises it.

`NUAH_JNI_CHECK=1` enables ART's `-Xcheck:jni` verifier for façade debugging;
normal launches leave it off because it adds work to every JNI transition.
Turbo disables the packaged `FFlagSlowDownRendering` value and gives
AssetProvider workers render-priority by default. Set `NUAH_FAST_RENDER=0` or
`NUAH_ASSET_BACKGROUND=0` to restore the packaged scheduling for an A/B run;
the corresponding `=1` values force the desktop policy on non-turbo modes.
These settings cannot remove stalls caused by Roblox's remote asset requests or
the host's refresh rate.

On four-thread/low-end hosts the tested RIVALS profile leaves Roblox's
auxiliary RbxTransport DummyClient enabled. Setting
`NUAH_DISABLE_RBX_TRANSPORT_DUMMY=1` can trigger the server's error-257
authentication rejection; use `0` for normal play. `NUAH_FRM_QUALITY=1..21` is an opt-in
engine-owned quality/LOD A/B control for scenes that remain CPU/GPU bound.

The turbo local profile targets the host's 60-Hz compositor by default and
automatically leaves one logical CPU free for ART, Wayland, and the Vulkan
driver. On the measured Intel UHD 620, the default (or
`NUAH_PERFORMANCE_MODE=turbo`) selects a
720x405 Vulkan surface, a 60-FPS scheduler target, and FRM quality level 1;
that profile sustains 60 FPS with substantially fewer long frame gaps in a
populated room, while 960x540 can drop to 30--45 FPS. Use
`NUAH_PERFORMANCE_MODE=balanced` for the larger 960x540 surface,
or `quality` for the full requested size. Set `NUAH_FRM_QUALITY=0` with turbo
to keep the packaged quality, or choose `1..21` explicitly. `NUAH_TASK_THREADS=<n>` overrides
both Roblox's automatic task limit and its in-game worker count, and
`NUAH_TARGET_FPS=<n>` (30--240) is available for high-refresh hosts or
profiling. A 70-FPS target is useful only on a display above 60 Hz; on this
machine it adds scheduler pressure without increasing visible frames. An
explicit `NUAH_CLIENT_SETTINGS_JSON` or `NUAH_CLIENT_SETTINGS_PATH` remains
authoritative and may provide its own `DFIntTaskSchedulerTargetFps` value.

Nuah also holds `nuah-runtime.lock` in the selected `--data` directory for the
whole native session. Launching a second room against the same cache is
rejected instead of allowing two AssetProvider instances to lock and recover
`rbx-storage.db` on the render thread. Closing the launcher forwards its
signal to the isolated Roblox child, so the lock and SQLite handles are
released together.

Roblox already requests KTX2 texture representations and performs the Basis
transcode/BCn upload itself. Nuah does not duplicate that decoder. Packaged
APK members are kept in a bounded, single-flight cache so repeated framework
and texture probes do not rescan/decompress the same ZIP member on the render
path. On small hosts, `NUAH_RENDER_TEXTURE_BUDGET_MS=4` limits Roblox's
per-frame texture-processing budget, while `NUAH_ASSET_PROVIDER_THREADS=1`
limits its cache-read and callback workers; these values are selected
automatically on four-thread hosts (explicit values still win).
`NUAH_ASSET_DISK_CACHE=1` opts into persistent Roblox disk-cache reads for
repeat visits; unset preserves the packaged Android cache policy. These
controls do not transcode or rewrite texture data. Roblox requests KTX2 and
performs the Basis-to-BCn upload on its own worker, so Nuah does not add a
second decoder or move that work onto the render thread. `NUAH_INPUT_COALESCE=0` disables the
default per-pump relative-pointer coalescing for raw input profiling;
coalescing preserves the accumulated mouse delta while reducing redundant
JNI/FunctionMarshal calls.

Mesa's shader cache is persistent per Nuah profile and defaults to a 1 GiB
limit so a populated room can reuse compiled pipelines across launches.
Override it with `NUAH_SHADER_CACHE_MAX_SIZE` (or disable it with
`NUAH_SHADER_CACHE=0`) when disk space matters.
On hosts with four or fewer logical CPUs, Nuah also enables Mesa ANV's
dedicated submit thread by default. This keeps i915 command submission off
Roblox's FunctionMarshal callback and reduces long `vkAcquireNextImageKHR`
stalls. Set `NUAH_VULKAN_SUBMIT_THREAD=0` to disable it, `1` to force it, or
`auto` to use the host default.

On the measured four-thread/Intel iGPU profile, Nuah clears the packaged
`FFlagSimRuntimeContentTranscodeBlockingCall` flag by default. This only asks
Roblox to finish its existing KTX2/Basis work asynchronously; Nuah does not
ship a second transcoder or rewrite texture formats. Set
`NUAH_ASSET_TRANSCODE_ASYNC=0` to restore the packaged blocking behavior for an
A/B comparison, or set it to `1` to force the non-blocking policy on a larger
host.

Nuah does not apply the APK's full mobile `applicationSettings` response by
default: those settings are battery-oriented and reduced the populated-room
cadence on the Intel iGPU. The small host scheduler/graphics settings are used
instead; set `NUAH_USE_PACKAGED_CLIENT_SETTINGS=1` to reproduce Android's
policy. Non-critical profile HTTP 429 responses also fail fast by default so a
three-second retry queue cannot stop presents; set
`NUAH_HTTP_BACKGROUND_NO_RETRY=0` to restore Android retry behavior.

Before ART starts, Nuah also checkpoints a non-empty `rbx-storage.db-wal`
under the profile lock. This prevents SQLite from replaying a stale cache WAL
on the first render-adjacent open after an interrupted session. The host
SQLite DSO is used only for this preflight and is never exposed to Android
code; `NUAH_CACHE_CHECKPOINT=0` disables it for comparison.

The requested mode is only moved to the front when the host driver advertises
it; Nuah never reports an invented mode.  `NUAH_PERF_TRACE=1` enables one-line
per-second summaries for Vulkan presents, SDL input-pump time, and JNI key or
pointer dispatch latency.  It is disabled by default and does not add a
logging or locking path to normal gameplay.  For system-level attribution,
attach `perf` to the native child after the room is visible:

```sh
perf stat -p "$(pgrep -n -f './build/nuah native-run')" \
  -e cycles,instructions,context-switches,cache-misses -I 1000
```

Compare the same room and camera movement for each mode.  A high Roblox
userspace sample share with a stable present interval points at game workload;
large present intervals or bridge latency point at the WSI/input boundary.

Native ART uses the host's maintained Java trust store when one is available
(`/etc/pki/java/cacerts` on Fedora/RHEL, or the Debian/Alpine equivalents), so
Roblox's HTTPS pre-warm does not fall back to an empty Android `/system` store.
`NUAH_JAVA_TRUST_STORE=<file>` selects another JKS; set
`NUAH_DISABLE_JAVA_TRUST_STORE=1` only when diagnosing a custom provider.  On
hosts without a working IPv6 route, the Android Bionic façade narrows ordinary
`AF_UNSPEC` lookups to IPv4 so OkHttp cannot spend repeated 10-second connect
timeouts on dead AAAA addresses.  Explicit IPv6 requests are unchanged;
`NUAH_PREFER_IPV4=0` disables this host workaround.

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
