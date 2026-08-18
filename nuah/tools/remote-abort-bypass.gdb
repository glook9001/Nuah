set pagination off
set confirm off
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
set environment LD_LIBRARY_PATH /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/build:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/art:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/hybris/lib:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/build/ispc:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/atl-bionic:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/vulkan:/home/niggermonkey/.local/share/nuah/base.apk_/lib
set environment LD_PRELOAD /usr/lib64/libpng16.so.16:/usr/lib64/libjpeg.so.62:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/art/libandroidfw.so
set environment NUAH_GRAPHICS_BACKEND vulkan
set environment NUAH_TASK_THREADS 1
set environment NUAH_BOOTSTRAP_TRACE 1
set environment NUAH_DISABLE_RBX_TRANSPORT_DUMMY 1
break abort
commands
  silent
  # libc's abort has already adjusted its own stack when GDB stops there;
  # the libroblox return address is one slot above the current RSP.
  set $ret = *(void **)($rsp + 8)
  set $base = $ret - 0x2983705
  # The failing Roblox allocator request is 0x80 bytes.  Supplying a real
  # host allocation here is only a diagnostic bridge; it avoids handing the
  # allocator a fabricated global arena pointer.
  set $arena = (void *)malloc(0x80)
  if $arena == 0
    printf "abort bypass: host malloc failed\n"
    quit
  end
  set $rax = $arena
  set $r15 = 0x70
  set $rsp = $rsp + 16
  set $pc = $base + 0x2983652
  continue
end
handle SIGABRT stop print nopass
catch signal SIGSEGV
commands
  silent
  printf "BYPASS SIGSEGV pc=%p rsp=%p rax=%p rdi=%p rsi=%p rdx=%p r15=%p\n", $pc, $rsp, $rax, $rdi, $rsi, $rdx, $r15
  x/8i $pc
  bt 12
  quit
end
run native-run --width 1220 --height 980 --apk /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/base.apk --split /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/split_config.x86_64.apk --data /home/niggermonkey/.local/share/nuah --uri roblox://placeId=17625359962
