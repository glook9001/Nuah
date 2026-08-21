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
cmake --build build --target nuah nuah-services
```

| Output | Source |
|---|---|
| `nuah`, `nuah-services` | `nuah/src/` |
| `android/libvulkan.so` | `nuah/android_shims/vulkan.cpp` |
| `android/libbionic.so` | `bionic.cpp` + asm |
| `android/libandroid.so` | `platform.cpp`, `egl.cpp`, … |
| other `android/*.so` | remaining shims |
| `android/linker-deps/*.so` | `linker_stub.c` (DT_NEEDED placeholders) |
| `libpthread_bio.so` | `third_party/bionic_translation/pthread_wrapper/libpthread.c` |

JNI core is the android2gnulinux submodule (`jvm.c`, `libjvm-java.c`,
`wrapper.c`). Headers: `third_party/libnativehelper`.

libhybris is **not** a CI artifact. Use the local install
(`~/.local/share/nuah/hybris`) built from `third_party/libhybris.lock` and
`nuah/libhybris_patches/0002-expose-builtin-hook-resolution.patch`.
Host ART is `/usr/local/lib64/art`.

`nuah/helper/` is only for `NUAH_NATIVE_BIONIC_SMOKE=1` (NDK + linker64).
A normal game launch does not use it.
