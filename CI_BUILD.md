# How Nuah is compiled

Two GitHub workflows own the compile contract. A local `cmake --build`
does **not** produce every `.so` that a Roblox launch needs. The Android-ELF
providers and the API-36 linker bundle are built or extracted in CI.

## Workflows

| Workflow | File | What it produces |
|---|---|---|
| Extract API-36 bionic core | `.github/workflows/bionic-core.yml` | `libc.so`, `libdl.so`, `libm.so`, `linker64` from the Android 36 system image |
| Nuah native runtime | `.github/workflows/nuah-native.yml` | libhybris, host Nuah, Android helper `.so` files, published `nuah-native-runtime-x86_64` |

`NUAH_BUILD_ATL` is **OFF** in CI. The in-tree ATL/wolfSSL/bhook trees are not compiled.

## Host CMake (`cmake -S . -B build -DNUAH_BUILD_ATL=OFF`)

These are **host** ELF objects (glibc). They live under `build/` / `build-ci/`.

| Output | Source |
|---|---|
| `nuah`, `nuah-services` | `nuah/src/main.cpp`, `nuah/src/services_main.cpp` + `nuah_core` |
| `libnuah_host_bridge.so` | `nuah/src/native_window_bridge.cpp` |
| `libnuah_atl_overlay.so` | `nuah/atl_overlay/compat.cpp` |
| `android/libbionic.so` | `nuah/android_shims/bionic.cpp` + asm |
| `android/libbionic-linker-helpers.so` | `nuah/android_shims/bionic_linker_helpers.cpp` |
| `android/liblog.so` | `nuah/android_shims/log.cpp` |
| `android/libm.so` | `nuah/android_shims/math.cpp` |
| `android/libandroid.so` | `asset_manager.cpp`, `egl.cpp`, `looper_fast.cpp`, `platform.cpp` |
| `android/libvulkan.so` | `nuah/android_shims/vulkan.cpp` |
| `android/libmediandk.so`, `libOpenSLES.so`, `libOpenMAXAL.so` | `platform.cpp` (SONAME aliases) |
| `android/libnuah_android_registry.so` | `nuah/android_shims/registry.cpp` |
| `android/linker-deps/*.so` | `nuah/android_shims/linker_stub.c` (empty DT_NEEDED placeholders) |
| `bionic-translation/libpthread_bio.so` | `third_party/bionic_translation/pthread_wrapper/libpthread.c` |
| `libnuah_ispc_asset.so` | `nuah/ispc/asset_pack.ispc` (optional) |

CMake also compiles these JNI files from **your** android2gnulinux fork (static, not a `.so`):

- `third_party/android2gnulinux/src/jvm/jvm.c`
- `third_party/android2gnulinux/src/libjvm-java.c`
- `third_party/android2gnulinux/src/wrapper/wrapper.c`

JNI headers come from `third_party/libnativehelper/include_jni`.

## CI-only Android ELF (NDK, not host CMake)

`.github/workflows/nuah-native.yml` compiles these with
`x86_64-linux-android35-clang` so **linker64** can load them. Do not delete
`nuah/helper/` — CMake does not list them as libraries, but CI publishes them
into `build-ci/bionic/lib64/`.

| Output | Source |
|---|---|
| `nuah-bionic-loader` | `nuah/helper/bionic_loader_main.c`, `nuah/helper/jni_facade.c` |
| `lib64/libEGL.so` | `nuah/helper/graphics_proxy.c` (`-DNUAH_EGL_PROXY`) |
| `lib64/libGLESv2.so` | `nuah/helper/graphics_proxy.c` (`-DNUAH_GLES_PROXY`) |
| `lib64/libandroid.so` | `nuah/helper/platform_proxy.c` |
| `lib64/liblog.so` | same |
| `lib64/libmediandk.so` | same |
| `lib64/libOpenSLES.so` | same |
| `lib64/libOpenMAXAL.so` | same |

Libhybris smoke probes (also CI-only, host `cc`):

- `nuah/tests/hybris_probe_module.c`
- `nuah/tests/hybris_host_dependency_probe.c`
- `nuah/tests/hybris_loader_probe.c`

## Extracted, not compiled

`bionic-core.yml` pulls the API-36 Google system image and publishes:

- `lib64/libc.so`
- `lib64/libdl.so`
- `lib64/libm.so`
- `lib64/linker64`

The native-runtime job downloads a **pinned** artifact of that bundle
(`gh run download 30408623995`). It is not rebuilt on every push.

## libhybris (cloned in CI, not vendored)

`third_party/libhybris.lock` pins `libhybris/libhybris.git` at revision
`7079712a42ea2754adf747e70c6cc75764c8596e`. CI applies
`nuah/libhybris_patches/0002-expose-builtin-hook-resolution.patch` and builds:

- `lib/libhybris-common.so`
- `lib/libhybris/linker/q.so`

Headers: `nuah/hybris_headers/`. Local launches use the installed copy under
`~/.local/share/nuah/hybris/`.

## Trees this compile does **not** use

Removed from Git: full ATL, wolfSSL, bhook, packaging. Host ART/AOSP checkouts
stay gitignored local dumps, not this repo.

Keep `third_party/bionic_translation/` (patched; `libpthread_bio` and the
Meson `libdl_bio` pin), the android2gnulinux **submodule**, `nuah/helper/`,
and the hybris probe sources.

## Local product build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNUAH_BUILD_ATL=OFF
cmake --build build --target nuah nuah-services
```

That is enough for `./build/nuah config` / `native-run` on a machine that
already has API-36 ART, hybris, and the NDK helper `.so` files installed.
Rebuilding the Android-ELF helpers requires the NDK command from
`nuah-native.yml` (search for `x86_64-linux-android35-clang`).
