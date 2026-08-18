set pagination off
set confirm off
set disable-randomization on
set follow-fork-mode child
set detach-on-fork off
set breakpoint pending on
hbreak *0x7fffc5caff6e
commands
  silent
  printf "HIT arena allocator rdi=%p rsi=%p rdx=%p rcx=%p\n", $rdi, $rsi, $rdx, $rcx
  continue
end
break abort
commands
  silent
  printf "ABORT caller=%p\n", *(void **)($rsp + 8)
  quit
end
handle SIGABRT stop print nopass
set environment XDG_RUNTIME_DIR /run/user/1000
set environment WAYLAND_DISPLAY wayland-0
set environment DBUS_SESSION_BUS_ADDRESS unix:path=/run/user/1000/bus
set environment NUAH_ATL_NATIVE_DIR /home/niggermonkey/.local/share/nuah/base.apk_/lib
set environment NUAH_ATL_LIBRARY_DIR /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/atl-bionic
set environment NUAH_HYBRIS_LIBRARY /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/hybris/lib/libhybris-common.so
set environment HYBRIS_LINKER_DIR /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/hybris/lib/libhybris/linker
set environment LD_LIBRARY_PATH /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/art:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/hybris/lib:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/build/ispc:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/atl-bionic:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/vulkan
set environment LD_PRELOAD /usr/lib64/libpng16.so.16:/usr/lib64/libjpeg.so.62:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/art/libandroidfw.so
set environment NUAH_GRAPHICS_BACKEND vulkan
set environment NUAH_TASK_THREADS 1
set environment NUAH_BOOTSTRAP_TRACE 1
set environment NUAH_DISABLE_RBX_TRANSPORT_DUMMY 1
run native-run --width 1220 --height 980 --apk /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/base.apk --split /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/split_config.x86_64.apk --data /home/niggermonkey/.local/share/nuah --uri roblox://placeId=17625359962
