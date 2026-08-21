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
relative mouse movement, buttons, and wheel events. The playable path uses
the libhybris loader with the installed API-36 ART provider.

## Build and run

```sh
cmake -S . -B build -G Ninja
cmake --build build --target nuah nuah-services
./build/nuah config
```

### Launch RIVALS with the current Sober APK

The recommended launch path is the worker script. It discovers the current
Sober APK and split, adopts Sober's active login cookie, and supplies the
join parameters required by current Roblox APKs:

```sh
cd ~/Documents/nuah
cmake --build build --target nuah nuah-services -j"$(nproc)"
./nuah/tools/run-rivals-worker-ab.sh 0 1280 720
```

Start Sober and sign in first. The script uses the current package at:

```text
~/.var/app/org.vinegarhq.Sober/data/sober/packages/x86_64/com.roblox.client/
```

Do not replace the current APK's `libroblox.so` with an older extracted copy.
The current client is identified in the log by its `RobloxGitHash` and must be
run with its matching `ExtraContent` assets. A successful launch contains:

```text
NetworkClient:Create
Connection accepted
onGameLoaded: placeId:17625359962
```

For the older APK saved under `~/Documents/sober/dist/apk`, use explicit
overrides when testing it:

```sh
NUAH_APK_PATH=~/Documents/sober/dist/apk/base.apk \
NUAH_SPLIT_APK_PATH=~/Documents/sober/dist/apk/split_config.x86_64.apk \
./nuah/tools/run-rivals-worker-ab.sh 0 1280 720
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
export NUAH_HYBRIS_LIBRARY="$HOME/.local/share/nuah/hybris/lib/libhybris-common.so"
export HYBRIS_LINKER_DIR="$HOME/.local/share/nuah/hybris/lib/libhybris/linker"
export LD_LIBRARY_PATH=/usr/local/lib64/art:"$HOME/.local/share/nuah/hybris/lib"
export LD_PRELOAD=/usr/lib64/libpng16.so.16:/usr/lib64/libjpeg.so.62:/usr/local/lib64/art/libandroidfw.so
export NUAH_GRAPHICS_BACKEND=vulkan
export NUAH_PERFORMANCE_MODE=turbo
export NUAH_VULKAN_PRESENT_MODE=fifo
export NUAH_VULKAN_SUBMIT_THREAD=0
export MESA_VK_ENABLE_SUBMIT_THREAD=0
export NUAH_INPUT_COALESCE=1
export NUAH_NONBLOCK_WAYLAND_EVENTS=0
export NUAH_MOUSE_CAPTURE=1
export NUAH_FAST_RENDER=1
export NUAH_ASSET_BACKGROUND=1
export NUAH_TASK_THREADS=0
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

### Verify the RIVALS join

The launcher log should contain all of the following after startup:

```text
Transport selection: useRbxTransportEnabled=false, selectedTransport=RakNet
onGameLoaded: placeId:17625359962
```

RakNet plus `onGameLoaded` is the success criterion, not merely that a Roblox
window appeared.

If Roblox reports **error 257**, first start Sober and confirm that its current
session is valid. Do not add protocol or unrelated client-setting overrides.

An `AssetDelivery403IncorrectAssetType` or `Asset type does not match requested
type` entry can be a remote game-asset failure after `onGameLoaded`. It is not
an authentication or transport failure and does not mean the RIVALS join failed.

The launch flags above are the tested play profile. `NUAH_GRAPHICS_BACKEND`,
`NUAH_PERFORMANCE_MODE`, `NUAH_VULKAN_PRESENT_MODE`,
`NUAH_VULKAN_SUBMIT_THREAD`, `MESA_VK_ENABLE_SUBMIT_THREAD`,
`NUAH_INPUT_COALESCE`, `NUAH_NONBLOCK_WAYLAND_EVENTS`, `NUAH_MOUSE_CAPTURE`,
`NUAH_FAST_RENDER`, `NUAH_ASSET_BACKGROUND`, `NUAH_TASK_THREADS`,
`NUAH_RENDER_TEXTURE_BUDGET_MS`, `NUAH_TARGET_FPS`, and
`NUAH_SHADER_CACHE_DIR` control rendering, scheduling,
input, and caching. `NUAH_HYBRIS_LIBRARY`,
`HYBRIS_LINKER_DIR`, `LD_LIBRARY_PATH`, and `LD_PRELOAD` select the Android
runtime boundary. `NUAH_ROBLOX_COOKIES` and `NUAH_ROBLOX_COOKIE_HEADER` pass
the Sober session to Roblox. An explicit `NUAH_CLIENT_SETTINGS_JSON` still
overrides generated scheduler/graphics keys, so keep it unset unless you are
A/B testing a specific flag.

For a controlled input-causality A/B, set `NUAH_DROP_MOUSE_MOTION=1` for one
run. Nuah will continue processing pointer-lock state and button/wheel events,
but will not forward mouse-motion deltas to Roblox. A material improvement in
the same room would implicate Roblox's motion-driven camera/render path; no
change would point back to the Vulkan/resource hitches instead. This is a
diagnostic switch only and is intentionally unset for normal play.

### Hitch measurement and descriptor experiment

The normal play profile leaves `NUAH_ENGINE_TRACE` and
`NUAH_FRAME_WORK_TRACE` unset. They take a mutex and periodically format
diagnostic output on the render thread, so enabling them can manufacture a
hitch. Use them only for a bounded capture. The supported root probe is:

```sh
pkexec timeout --signal=INT --kill-after=2s 15s \
  bpftrace "$PWD/nuah/tools/bpftrace-hitch-work.bt" <nuah-child-pid>
```

To distinguish time spent inside the implicated ANV routines from time where
the render thread is merely sampled in them, use the bounded duration
histogram (with the system Intel Mesa driver):

```sh
pkexec timeout --signal=INT --kill-after=2s 20s \
  bpftrace "$PWD/nuah/tools/bpftrace-engine-duration.bt" <nuah-child-pid>
```

On the tested steady route, descriptor allocation, BLORP, Gen9 runtime-state
packing, draw recording, and `i915_queue_exec_locked` were normally in the
microsecond-to-low-millisecond range. This means a 100 ms sampled stack is
not by itself proof that the named function consumed 100 ms; page faults,
queue waits, and residency work can suspend the thread while it is inside the
call chain.

`NUAH_DESCRIPTOR_BIND_DEDUP=1` is an opt-in safety experiment. It suppresses
only an exact repeated descriptor bind within one command buffer and is
invalidated at begin/reset/free/secondary-command boundaries. It does not
cache or fabricate descriptor sets. The current RIVALS route produced zero
eligible repeats, so it is not part of the play profile. Do not suppress
`madvise`, descriptor writes, barriers, or Vulkan copies globally: those calls
carry application lifetime and synchronization semantics.

`NUAH_DESCRIPTOR_ALLOC_BATCH=4` is a separate descriptor allocator experiment.
It preallocates a bounded batch of real same-layout sets from Roblox's own
pool and returns cached handles on later allocations. Allocations with a
`pNext` chain are bypassed, and pool reset/destroy/device teardown invalidate
the cache. `NUAH_DESCRIPTOR_ALLOC_TRACE=1` reports requests, driver calls,
cache hits, and retained batch sets. On the tested 1280x720 RIVALS lobby route,
batch 4 produced roughly 46% cache hits and removed the old per-request
allocator pressure without a pool-allocation error; it is still an opt-in
profile because pool capacity and route behavior vary. The reproducible
launcher accepts these variables directly. `NUAH_COMMAND_STATE_DEDUP=1`
similarly tests exact repeated pipeline/index/vertex binds. The shim keeps one
exact snapshot for each bind kind per command buffer, so interleaved
pipeline/index/vertex traffic can still be recognized without assuming that
one kind invalidates another. It is disabled by default and should remain
opt-in unless its trace shows useful suppression on the selected route; the
measured route suppressed roughly 3--6% of these redundant state calls, but
that alone does not remove driver residency or page-fault hitches.

The current libroblox allocator first tries `MADV_FREE` and, for the mapped
allocator range used by this APK, receives `EINVAL` before retrying with
`MADV_DONTNEED`. `NUAH_LIBROBLOX_MADVISE_PATCH=1` enables a build-signature-
guarded in-memory patch that changes only the allocator's default advice from
8 to 4, removing that guaranteed failed syscall while preserving the retry's
effective `DONTNEED` behavior. It refuses to patch if the exact call/constant
pattern is absent or ambiguous. This is independent of the cgroup reclaim
profiles below and is disabled by default.

The launcher also passes `NUAH_VULKAN_SUBMIT_THREAD` through unchanged. Use
`1` only for a same-route A/B: it moves ANV queue submission off the Roblox
render thread, but can add CPU contention on a four-thread machine. The
documented profile remains `0` because the matched baseline used fewer CPU
seconds with submission on the render thread; this does not make the 50–170 ms
driver/residency stalls disappear.

### Experimental Intel texture LOD clamp

`NUAH_TEXTURE_MIN_LOD=1` is an opt-in Vulkan sampler policy for bandwidth
experiments. It preserves the selected framebuffer resolution and APK assets,
but makes mipmapped samplers start one mip lower; samplers with no mip chain
are unchanged. It has reached RIVALS' `onGameLoaded` and the shim has verified
that Roblox's real sampler calls are adjusted. It is not enabled by default,
does not reduce upload/transcode work, and has no claimed FPS gain until it is
measured on the same room and camera route. Use `NUAH_TEXTURE_MIN_LOD_TRACE=1`
only to log the first eight adjusted samplers during a diagnostic launch.

### Experimental Gen9 potato texture cache

For Intel Gen9 iGPUs, `NUAH_TEXTURE_SIDECAR=1` with the reproducible launcher
builds a separate **potato4** app-data profile and launches RIVALS from it:

```sh
NUAH_TEXTURE_SIDECAR=1 nuah/tools/run-rivals-worker-ab.sh 4 1280 720
```

The builder finds RBXH-wrapped ETC2 KTX2 BLOBs in Roblox's `RbxStorage` SQLite
cache, removes the four largest mip levels when a complete remaining mip chain
exists, and preserves the KTX2 format, DFD/KVD/SGD metadata, compression, and
opaque RBXH envelope. It does not transcode ETC2 at runtime. On the tested
cache it transformed 7,266 records from 17.7 MB to 6.33 MB of stored texture
payload. The active game process uses a Btrfs copy-on-write clone under
`~/.local/share/nuah/nuah-texture-sidecar/`; the known-good Nuah/Sober cache
is not overwritten. The source database hash is part of the profile path, so
a refreshed Roblox cache builds a new profile rather than reusing stale data.

This intentionally trades sharpness for lower texture residency and upload
work. It is an asset-residency experiment, not a proven average-FPS gain: use
the same room/camera route and compare `nuah perf: vulkan` p95/p99 intervals.
Unset `NUAH_TEXTURE_SIDECAR` to return immediately to the original cache.
Do not combine it with `NUAH_TEXTURE_MIN_LOD`; the launcher sets that clamp to
zero for this profile to avoid double-degrading the retained mips.

For a memory-residency/CCS diagnostic, combine the sidecar with a per-process
no-swap cgroup and the Intel Gen9 CCS switch:

```sh
NUAH_NO_SWAP=1 NUAH_INTEL_NO_CCS=1 NUAH_TEXTURE_SIDECAR=1 \
NUAH_VULKAN_MIN_IMAGE_COUNT=4 \
nuah/tools/run-rivals-worker-ab.sh 0 1280 720
```

`NUAH_NO_SWAP=1` sets `MemorySwapMax=0` only for Nuah and its Roblox child; it
does not disable zram globally and can cause an OOM if the system is already
full. `NUAH_INTEL_NO_CCS=1` appends the process-local `INTEL_DEBUG=noccs`
diagnostic. It avoids Intel auxiliary-compression resolves at the cost of
more memory bandwidth and more GEM residency pressure; the measured RIVALS
comparison reached consecutive 59–60 FPS one-second windows with CCS enabled,
while the `noccs` run repeatedly fell into 30–46 FPS intervals. Keep it as an
A/B profile rather than a default.
`NUAH_MEMORY_LOW_MB=1536` is a separate opt-in systemd cgroup profile: it
protects up to that amount of active Nuah/Roblox memory from global reclaim at
the cost of leaving less reclaimable memory for other applications. It does
not suppress Roblox's `madvise` calls and should be compared on the same route.
For the specific reclaim-driven hitch path, `NUAH_MEMORY_MIN_MB=2048` sets a
stronger cgroup `memory.min` reservation. It preserves Roblox's
`madvise(MADV_DONTNEED)` semantics while asking the kernel not to reclaim the
scope's protected working set; this can OOM the scope if the reservation cannot
be met, so it is never enabled by default. Compare it with `NUAH_NO_SWAP=1`
and the same room/camera route, and inspect `memory.events` afterward.
Four swapchain images are intentional here: the extra image reduces FIFO
`vkAcquireNextImageKHR`/`drmSyncobjTransfer` stalls during streaming. Set the
value to `3` only for a direct comparison.

When `ispc` is installed at CMake configure time, Nuah also builds
`build/ispc/libnuah_ispc_asset.so`. The sidecar builder loads it automatically
and uses AVX2 (with SSE4 fallback) SPMD gangs to classify the complete SQLite
cache before parsing eligible KTX2 records. This runs only while preparing a
new hash-keyed profile, with no render-thread calls. To test an explicit
library or force the scalar fallback:

```sh
python3 nuah/tools/build-lossy-ktx-sidecar.py SOURCE.db DERIVED.db \
  --ispc-copy-lib build/ispc/libnuah_ispc_asset.so
# Scalar comparison: --ispc-copy-lib /nonexistent
```

The normal Release C++ build already uses `-O3`, which enables GCC's tree
vectorizer; ISPC is used where explicit SPMD batch work is clearer and more
reliable than hoping a scalar loop auto-vectorizes.

For a separate runtime A/B, ISPC can fingerprint mapped texture-upload bytes
before Nuah's already-guarded duplicate-upload experiment. It uses the AVX2
implementation above and only suppresses a copy when the same sampled image
subresource has the same fingerprint; color/depth/storage/transient targets
remain excluded. This is disabled by default because a fixed room/camera
visual comparison is required before treating it as a play profile:

```sh
NUAH_TEXTURE_UPLOAD_TRACE=1 \
NUAH_TEXTURE_UPLOAD_HASH_TRACE=1 \
NUAH_ISPC_UPLOAD_FINGERPRINT=1 \
NUAH_TEXTURE_UPLOAD_DEDUP=1 \
nuah/tools/run-rivals-worker-ab.sh 4 1280 720
```

The log reports `hashed_regions`, `identical_regions`, and
`suppressed_regions`. A successful RIVALS run suppressed 44 proven-identical
regions (~2 MiB) in one interval, but that is evidence of avoided work—not
yet a general FPS claim. Set `NUAH_TEXTURE_UPLOAD_DEDUP=0` to retain the
ISPC measurement while forwarding every upload.

`NUAH_VULKAN_COPY_TRACE=1` is also rate-limited to one line per second. The
copy command can be called once per mip level; unbounded `fprintf` logging on
that path can itself block the render thread and create a false hitch. Nuah
observes both `vkCmdCopyImage` (the legacy entry point Mesa maps internally to
`anv_CmdCopyImage2`) and `vkCmdCopyImage2`; these hooks are observational and
do not remove image copies because they may carry layout and ordering
semantics. The upload deduper remains opt-in: the measured A/B suppressed only
about 1 MiB while still showing multi-hundred-millisecond stalls, so it is not
part of the normal profile.

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

Nuah negotiates the four-image FIFO swapchain used by Sober in every render
quality mode. The extra queued image keeps the compositor fed while Roblox
streams a scene: on this Intel/Wayland host, the previous three-image queue
repeatedly blocked `vkAcquireNextImageKHR` in `drmSyncobjTimelineWait` for
20--25 ms. Set `NUAH_VULKAN_MIN_IMAGE_COUNT=3` only for a latency A/B; it
reintroduces that residency/pacing wait and is not the hitch-resistant
profile. The host driver's maximum is still honored, so the adjustment is
skipped when four images are unavailable.

### Hitch investigation plan

Use the same room/camera route and discard the first 30 seconds of each run
before comparing `p95`, `p99`, maximum present interval, and counts over
33/50/100 ms. The read-only hitch classifier is:

```sh
pkexec timeout --signal=INT --kill-after=2s 30s \
  bpftrace nuah/tools/bpftrace-hitch-work.bt <native-run-pid>
```

Current captures identify several separate costs rather than one broken
function: `blorp_copy`/`anv_device_map_bo` during buffer and image copies,
Gen9 CCS transitions, `gfx9_cmd_buffer_flush_descriptor_sets`, and
`i915_gem_do_execbuffer` page/residency work. Query-pool resets are normally
microsecond-scale and are not a justified suppression target. Kernel samples
also show that zram can amplify residency stalls, but a per-process no-swap
A/B still reproduced them, so disabling zram is not a complete fix.

Keep these mitigations opt-in until a matched route proves a tail improvement:
`NUAH_VULKAN_SUBMIT_THREAD=1`, `NUAH_DESCRIPTOR_ALLOC_BATCH=4`,
`NUAH_TEXTURE_UPLOAD_DEDUP=1` with its hash/trace prerequisites, and
`NUAH_NO_SWAP=1`. Upload hashing runs on the render thread and can itself
create a hitch; it must not be enabled merely because it suppresses copies.
The shipped profile therefore remains automatic workers (`0`), four FIFO
images, no upload hashing, and no descriptor batching. The launcher passes
`NUAH_VULKAN_SUBMIT_THREAD` through to Mesa instead of overwriting an explicit
A/B request.

`NUAH_JNI_CHECK=1` enables ART's `-Xcheck:jni` verifier for façade debugging;
normal launches leave it off because it adds work to every JNI transition.
Turbo disables the packaged `FFlagSlowDownRendering` value and gives
AssetProvider workers render-priority by default. Set `NUAH_FAST_RENDER=0` or
`NUAH_ASSET_BACKGROUND=0` to restore the packaged scheduling for an A/B run;
the corresponding `=1` values force the desktop policy on non-turbo modes.
These settings cannot remove stalls caused by Roblox's remote asset requests or
the host's refresh rate.

`NUAH_FRM_QUALITY=1..21` is an opt-in engine-owned
quality/LOD A/B control for scenes that remain CPU/GPU bound.

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
machine it adds scheduler pressure without increasing visible frames. Set
`NUAH_TASK_THREADS=0` to leave the worker count automatic (Nuah's host-based
default). `NUAH_TASK_THREADS=2` is deliberately ignored because it correlated
with long render hitches on the target Intel host; use `0` for automatic
selection or another value only for a measured A/B. An
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
The documented RIVALS launch disables Mesa ANV's dedicated submit thread for
the current diagnostic profile. This is not a general FPS recommendation: use
`NUAH_VULKAN_SUBMIT_THREAD=1`, `0`, or `auto` only for an A/B test on the same
room and camera route.

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
pkexec timeout --signal=INT --kill-after=2s 10s \
  perf stat -p "$(pgrep -n -f './build/nuah native-run')" \
  -e cycles,instructions,context-switches,cache-misses
```

Root-only `bpftrace` and `perf` captures should use the same bounded
`pkexec`/`timeout` pattern; it keeps the probe privilege scoped and prevents a
forgotten attachment from perturbing a later launch.

Compare the same room and camera movement for each mode.  A high Roblox
userspace sample share with a stable present interval points at game workload;
large present intervals or bridge latency point at the WSI/input boundary.

## Engine hotspot tracing and governor

`NUAH_ENGINE_TRACE=1` is the opt-in, whole-session renderer diagnostic. It
emits compact one-second summaries for Vulkan texture uploads, descriptor
allocation/binding, image-view creation, barriers, submits, waits, and
acquires. It also measures Android ABI mutex/condition waits. Synchronization
records include `caller_offset=0x...`, a module-relative libroblox return-PC
offset that can be inspected against the exact Sober APK image:

```sh
export NUAH_ENGINE_TRACE=1
nuah/tools/collect-engine-profile.sh  # discovers the running native child
nuah/tools/map-libroblox-offset.sh \
  "$HOME/.local/share/nuah/base.apk_/lib/libroblox.so" 0x247a4c4 --return-pc
```

Use the extracted `libroblox.so` path when the APK is not mounted as a
directory. `--return-pc` subtracts one before disassembly because sampled
return addresses may land immediately after an indirect `call`. Do not label a
caller as a spinlock from a sample alone: a high `mutex_lock` contention count
or condition-wait time is the evidence needed before considering a local patch.

`NUAH_ENGINE_GOVERNOR=balanced` and `throughput` apply only when Nuah generates
the Roblox client settings itself. They are not active in the documented RIVALS
launch because its explicit minimal client-settings JSON is required for the
transport handshake. `off` is the default.

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
