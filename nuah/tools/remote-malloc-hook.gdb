set pagination off
set confirm off
set disable-randomization on
set follow-fork-mode child
set detach-on-fork off
set breakpoint pending on
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
break abort
commands
  silent
  set $caller = *(void **)($rsp + 8)
  set $base = $caller - 0x2983705
  printf "first abort; installing malloc hooks at %p and %p\n", $base + 0x1ca6aa2, $base + 0x1ca797a
  break *($base + 0x1ca797a)
  commands
    silent
    set $size = $rsi
    set $mem = (void *)malloc($size + 0x100)
    if $mem == 0
      printf "arena malloc hook failed size=%lu\n", $size
      quit
    end
    set $retpc = *(void **)$rsp
    set $rax = $mem
    set $rsp = $rsp + 8
    set $pc = $retpc
    continue
  end
  break *($base + 0x1ca6aa2)
  commands
    silent
    set $size = $rdi
    set $mem = (void *)malloc($size)
    if $mem == 0
      printf "malloc hook failed size=%lu\n", $size
      quit
    end
    set $retpc = *(void **)$rsp
    set $rax = $mem
    set $rsp = $rsp + 8
    set $pc = $retpc
    continue
  end
  disable 1
  set $mem = (void *)malloc(0x80)
  if $mem == 0
    printf "initial malloc hook failed\n"
    quit
  end
  set $rax = $mem
  set $rsp = $rsp + 16
  set $pc = $base + 0x2983652
  continue
end
handle SIGABRT stop print nopass
run native-run --width 1220 --height 980 --apk /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/base.apk --split /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/split_config.x86_64.apk --data /home/niggermonkey/.local/share/nuah --uri roblox://placeId=17625359962
