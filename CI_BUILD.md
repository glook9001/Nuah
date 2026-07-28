# CI-only builds

The supported build machine is GitHub Actions. The `Android 16 ART/ATL build`
workflow produces two artifacts:

- `com.android.art-x86_64`: the Android ART APEX.
- `android16-host-art-x86_64`: Android 16 host ART libraries used by the ATL
  adapter.
- `android16-atl-runtime-x86_64`: produced by the manual Android 16 ABI gate
  once ATL links successfully against those libraries.

The Android 16 adapter gate is available through **Run workflow** in GitHub
Actions. It configures Nuah with the staged ART bootclasspath and builds the
Nuah binaries against that runtime; it does not use the low-power client.

On a low-power client, download the artifact from the Actions run and unpack
it. Do not run CMake or Meson locally. To download the latest successful run
with GitHub CLI:

```bash
gh run download --repo OWNER/REPOSITORY --name android16-atl-runtime-x86_64
```

Replace `OWNER/REPOSITORY` with the repository name. CI caching accelerates
subsequent builds, but it cannot reduce runtime input/render latency on the
client machine.

Install the downloaded archive without compiling locally:

```bash
bash tools/install_android16_runtime.sh android16-atl-runtime.tar.gz
```

When the Android 16 host runtime bundle is available, select it explicitly
without rebuilding Nuah:

```bash
export NUAH_ATL_RUNTIME=android16
export NUAH_ATL_ANDROID16_HOME=/path/to/android16-runtime
```

The bundle must contain `android-translation-layer` and a `natives/`
directory containing `libtranslation_layer_main.so`; Android 16 ART shared
objects belong in a sibling `lib/` directory, and `bootclasspath.txt` must
contain the split Android 16 bootclasspath (relative `java/...` entries are
resolved against the bundle). Alternatively set
`NUAH_ATL_ANDROID16_BOOTCLASSPATH` explicitly. Nuah refuses to launch unless
an Android 16 runtime is available.

Nuah also auto-selects Android 16 when the bundle is installed under
`/usr/local/lib64/nuah/android16-runtime`, `/usr/local/share/nuah/android16-runtime`,
or `/opt/nuah/android16-runtime`. Any non-Android-16 runtime value is
rejected.

The host adapter staging contract is implemented by
[`prepare_host_runtime.sh`](/home/pepe/Documents/sober/third_party/aosp_android16/prepare_host_runtime.sh).
