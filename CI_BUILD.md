# How Nuah is compiled

Cloud CI extracts the API-36 bionic core from Google's x86_64 system image.
Everything else is built on this machine.

## Cloud: API-36 bionic core

Workflow: `.github/workflows/bionic-core.yml` (manual **Run workflow**).

It downloads `system-images;android-36;google_apis;x86_64` and publishes
only what `native-run` needs from that image:

- `lib64/linker64`
- `lib64/libc.so`
- `lib64/libdl.so`
- `lib64/libm.so`

Artifact name: `nuah-bionic-core-api36-x86_64`.

```sh
gh run list --repo glook9001/Nuah --workflow 'Extract API-36 bionic core' --limit 3
gh run download <run-id> --repo glook9001/Nuah \
  --name nuah-bionic-core-api36-x86_64 --dir build/bionic
(cd build/bionic && sed 's# bionic-core/# #' SHA256SUMS | sha256sum --check)
```

Put that tree next to the local binary as `build/bionic/lib64/linker64`.

A known good extract is run `30408623995` if you do not want to re-download
the system image.

## Local: Nuah and host providers

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target nuah
```

| Output | Source |
|---|---|
| `nuah` | `nuah/app/`, `nuah/core/` |
| `android/libvulkan.so` | `nuah/gfx/vulkan.cpp` |
| `android/libbionic.so` | `platform/bionic.cpp` + asm |
| `android/libandroid.so` | `platform/platform.cpp`, `gfx/egl.cpp`, … |
| other `android/*.so` | remaining shims |
| `android/linker-deps/*.so` | `linker_stub.c` (DT_NEEDED placeholders) |
| `libpthread_bio.so` | `nuah/platform/pthread_bridge.c` |

Java execution is host ART (`libart.so` via `JNI_CreateJavaVM`); the owned
JNI façade lives in `nuah/core/roblox_java_facade.c`. Sole vendored header:
`nuah/vendor/libnativehelper-include_jni/jni.h` (modern AOSP declarations).

libhybris is **not** a CI artifact. Use the local install
(`~/.local/share/nuah/hybris`) built from `third_party/libhybris.lock` and
`third_party/libhybris_patches/0002-expose-builtin-hook-resolution.patch`.
Host ART is `/usr/local/lib64/art`.
