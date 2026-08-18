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
set environment LD_PRELOAD /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/build/android/libbionic.so:/usr/lib64/libpng16.so.16:/usr/lib64/libjpeg.so.62:/home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/art/libandroidfw.so
set environment NUAH_GRAPHICS_BACKEND vulkan
set environment NUAH_TASK_THREADS 4
set environment NUAH_BOOTSTRAP_TRACE 1
set environment NUAH_LIBROBLOX_ALLOCATOR_TLS_PATCH 1
set environment NUAH_PTHREAD_NAMESPACE 1
unset environment NUAH_PTHREAD_TLS_GUARD
set environment NUAH_ANDROID_PRELOAD /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/build/android/libbionic.so
set environment NUAH_ANDROID_RUNTIME compat
break abort
commands
  silent
  printf "ABORT pc=%p rbx=%p r12=%p r13=%p r14=%p r15=%p rsp=%p ret=%p\\n", $pc, $rbx, $r12, $r13, $r14, $r15, $rsp, *(void **)$rsp
  x/8gx $rbx
  up 1
  printf "CALLER rbx=%p r14=%p r15=%p r12=%p r13=%p\\n", $rbx, $r14, $r15, $r12, $r13
  x/16gx $rbx
  set $base = $pc - 0x2983705
  printf "allocator key=%x arena=%p init=%x tls=%p\\n", *(int *)($base + 0x6e69040), *(void **)($base + 0x6e05c80), *(int *)($base + 0x6e68b88), (void *)pthread_getspecific(*(int *)($base + 0x6e69040))
  printf "initobj=%p initflag=%p initptr=%p\\n", *(void **)($base + 0x6e67c40), (void *)($base + 0x6e67c58), *(void **)($base + 0x6e67c58)
  printf "abort-object=%p key2=%x\\n", (void *)($base + 0x6e1c570), *(int *)($base + 0x6e7b408)
  x/12gx ($base + 0x6e1c570)
  x/16gx ($base + 0x6e67c40)
  x/32gx ($base + 0x6e67c80)
  printf "getspecific GOT=%p PLT=%p\\n", *(void **)($base + 0x6e119c8), (void *)($base + 0x69595a0)
  info symbol *(void **)($base + 0x6e119c8)
  x/12bx ($base + 0x20adce7)
  x/24gx *(void **)($base + 0x6e05c80)
  x/24gx (void *)pthread_getspecific(*(int *)($base + 0x6e69040))
  x/24gx (void *)($base + 0x6e67c40)
  down 1
  bt 8
  quit
end
handle SIGABRT stop print nopass
run native-run --width 1220 --height 980 --apk /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/base.apk --split /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/split_config.x86_64.apk --data /home/niggermonkey/.local/share/nuah --uri roblox://placeId=17625359962
