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
set environment LD_LIBRARY_PATH /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/art:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/hybris/lib:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/build/ispc:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/atl-bionic:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/vulkan
set environment LD_PRELOAD /usr/lib64/libpng16.so.16:/usr/lib64/libjpeg.so.62:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/art/libandroidfw.so
set environment NUAH_GRAPHICS_BACKEND vulkan
set environment NUAH_TASK_THREADS 1
set environment NUAH_BOOTSTRAP_TRACE 1
set environment NUAH_DISABLE_RBX_TRANSPORT_DUMMY 1
break mmap
commands
  silent
  if $rsi >= 0x100000
    printf "MMAP request addr=%p len=0x%lx prot=0x%lx flags=0x%lx fd=%ld off=0x%lx\n", $rdi, $rsi, $rdx, $rcx, $r8, $r9
    finish
    printf "MMAP return=%p errno=%d\n", $rax, *(int *)__errno_location()
  end
  continue
end
break abort
commands
  silent
  printf "ABORT caller=%p rax=%p rdi=%p rsi=%p r12=%p r13=%p r14=%p r15=%p\n", *(void **)($rsp + 8), $rax, $rdi, $rsi, $r12, $r13, $r14, $r15
  quit
end
handle SIGABRT stop print nopass
run native-run --width 1220 --height 980 --apk /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/base.apk --split /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/split_config.x86_64.apk --data /home/niggermonkey/.local/share/nuah --uri roblox://placeId=17625359962
