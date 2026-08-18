# Nuah

Linux desktop runtime for Roblox Android APKs using native translation and Vulkan rendering.

## Quick Start

### 1. Build Nuah
```sh
cmake -S . -B build -G Ninja -DNUAH_BUILD_ATL=OFF
cmake --build build --target nuah nuah-services
./build/nuah config
```

### 2. Launch RIVALS
Make sure you have launched Sober once and logged into your Roblox account so session cookies and the APK package are present in `~/.var/app/org.vinegarhq.Sober/data/sober/`.

Then launch using the automated runner:
```sh
./nuah/tools/run-rivals-worker-ab.sh 0 1220 920
```

The automated runner handles session cookie/user ID adoption, APK asset extraction (`content`, `ExtraContent`, `fonts`, `shaders`), Vulkan Wayland presentation, and game server connection.

### 3. Remote Execution
To run Nuah on a secondary/remote host:
```sh
# 1. Sync binary and script to remote host
rsync -avz build/nuah nuah/tools/run-rivals-worker-ab.sh user@<remote-ip>:~/sober/

# 2. Launch over SSH
ssh user@<remote-ip> "cd ~/sober && \
  export DISPLAY=:0 \
  export WAYLAND_DISPLAY=wayland-0 \
  export XDG_RUNTIME_DIR=/run/user/\$(id -u) \
  ./nuah/tools/run-rivals-worker-ab.sh 0 1220 920"
```

For complete architectural details, manual configuration flags, and performance diagnostic options, see [nuah/README.md](file:///home/pepe/Documents/sober/nuah/README.md).
