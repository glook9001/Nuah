# Nuah Compilation, Packaging & GitHub Workflows Guide

Nuah is a native Linux runtime that runs the Roblox Android x86_64 native engine (`libroblox.so`) directly on Linux with native Vulkan graphics, an embedded ART 9 execution engine, a libhybris Bionic bridge, and a GTK4/Adwaita WebKit launcher.

---

## 1. Prerequisites & Dependencies

### Fedora (40, 41, 42, 43, 44)
```bash
sudo dnf install -y \
  cmake ninja-build gcc gcc-c++ ccache pkgconf-pkg-config \
  gtk4-devel libadwaita-devel webkitgtk6.0-devel \
  vulkan-loader-devel vulkan-headers libglvnd-devel \
  SDL3-devel libarchive-devel libseccomp-devel \
  glib2-devel openssl-devel libpng-devel libjpeg-turbo-devel \
  elfutils-libelf-devel libunwind-devel icu
```

### Ubuntu (24.04 LTS / 24.10 / Debian 13)
```bash
sudo apt-get update && sudo apt-get install -y \
  cmake ninja-build g++ ccache pkg-config \
  libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev \
  libvulkan-dev libsdl3-dev libarchive-dev libseccomp-dev \
  libglib2.0-dev libssl-dev libpng-dev libjpeg-dev \
  libelf-dev libunwind-dev libicu-dev
```

### Arch Linux
```bash
sudo pacman -S --needed \
  cmake ninja gcc ccache pkgconf \
  gtk4 libadwaita webkitgtk-6.0 \
  vulkan-headers vulkan-icd-loader libarchive libseccomp \
  glib2 openssl libpng libjpeg-turbo elfutils libunwind icu
```

---

## 2. Compiling the Binaries Locally

From the repository root:

```bash
# 1. Configure CMake with Ninja build generator
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# 2. Compile the supervisor (nuah) and WebKit login launcher (nuah-services)
ninja -C build nuah nuah-services
```

Output binaries will be placed in `build/`:
- `build/nuah` — Supervisor and native Android ELF loader.
- `build/nuah-services` — WebKitGTK authentication window and game browser.

---

## 3. GitHub Actions Workflows Guide

The repository includes automated CI/CD workflows under `.github/workflows/`:

| Workflow File | Purpose | Trigger |
| :--- | :--- | :--- |
| `.github/workflows/portable-release.yml` | Builds `nuah`, packages standalone `Nuah-Linux-x86_64.zip`, and creates GitHub releases. | Git tags (`v*`), push to `main`/`libhybris-pivot`, or manual trigger. |
| `.github/workflows/nuah-native.yml` | Builds `libhybris` loader from source, tests Bionic dynamic linker shims, and runs smoke probes. | Push to core paths or manual trigger. |
| `.github/workflows/bionic-core.yml` | Verifies and builds the Android API-36 Bionic core runtime environment. | Push or manual trigger. |

### How to Trigger Workflows

#### Option A: Via GitHub Web Interface
1. Go to the **Actions** tab on your GitHub repository (`https://github.com/glook9001/Nuah/actions`).
2. Select **Build Portable Release** from the left sidebar.
3. Click the **Run workflow** button on the right, select the branch (`libhybris-pivot` or `main`), and click **Run workflow**.

#### Option B: Via GitHub CLI (`gh`)
```bash
# Trigger the portable release workflow manually
gh workflow run portable-release.yml --ref libhybris-pivot

# Check status of running workflow
gh run list --workflow=portable-release.yml

# Watch live execution logs
gh run watch
```

#### Option C: Automated Release on Git Tag
Pushing a version tag triggers a build and automatically attaches `Nuah-Linux-x86_64.zip` to a new GitHub Release:

```bash
git tag v1.0.0
git push origin v1.0.0
```

### Downloading Workflow Artifacts

```bash
# Download the packaged portable bundle from the latest workflow run:
gh run download --name Nuah-Linux-x86_64 --dir ./release-artifacts

# Download prebuilt runtime DSOs (if updating libhybris or bionic core):
gh run download --name nuah-libhybris-x86_64 --dir dist/hybris
gh run download --name nuah-bionic-core-api36-x86_64 --dir dist/bionic
```

---

## 4. Sourcing Runtime Shared Libraries (`.so` DSOs)

The portable bundle embeds standalone Android runtime dependencies so that target machines do not need Android system files or Flatpak installed.

### Directory Structure
```text
dist/
├── art/                                # Standalone ART 9.0 DSOs (libart.so, libandroidfw.so, libbase.so, ...)
├── hybris/lib/                         # libhybris loader & linker (libhybris-common.so, libhybris/linker/q.so)
├── android/linker-deps/                # Android Bionic linker dependency placeholders
└── java/dex/
    ├── art/                            # Core DEX jars (core-oj-hostdex.jar, okhttp-hostdex.jar, ...)
    │   └── natives/                    # JNI companions (libjavacore.so, libopenjdk.so, libnativehelper.so)
    └── android_translation_layer/      # Android framework bytecode & assets (api-impl.jar, framework-res.apk)
```

### Building `libhybris` from Source Manually
```bash
# Read pinned upstream revision
. third_party/libhybris.lock

# Clone and patch
git clone --filter=blob:none "$repository" hybris-source
cd hybris-source
git checkout --detach "$revision"
git -C hybris apply --recount "$REPO_ROOT/nuah/libhybris_patches/0002-expose-builtin-hook-resolution.patch"

# Build for x86_64
cd hybris
autoreconf -vfi
./configure --enable-arch=x86-64 --with-android-headers="$REPO_ROOT/nuah/hybris_headers" --prefix="$REPO_ROOT/dist/hybris"
make -C common -j$(nproc)
make -C common install
```

---

## 5. Packaging the Standalone Release Bundle (`.zip` and `.tar.gz`)

Run the automated packaging script:

```bash
bash packaging/portable/build-portable.sh
```

This generates:
1. `dist-portable/Nuah-Linux-x86_64.zip` — Universal zip archive for easy distribution.
2. `dist-portable/Nuah-Linux-x86_64.tar.gz` — Tarball release for Linux package managers.
3. `dist-portable/nuah-portable/` — Extracted standalone directory ready to execute.

---

## 6. Running the Release

### On Any Linux Machine
```bash
# 1. Unpack
unzip Nuah-Linux-x86_64.zip
cd nuah-portable

# 2. Launch WebKit browser and sign in
./nuah

# Or launch a place directly
./nuah native-run --uri roblox://placeId=1818
```

### Transferring to Remote Machine
```bash
rsync -avz dist-portable/Nuah-Linux-x86_64.zip user@remote-host:~/Documents/
ssh user@remote-host "cd ~/Documents && unzip -o Nuah-Linux-x86_64.zip && cd nuah-portable && ./nuah"
```
