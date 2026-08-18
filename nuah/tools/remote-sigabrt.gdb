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
set environment LD_PRELOAD /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/build/android/libbionic.so
set environment NUAH_ANDROID_PRELOAD /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/build/android/libbionic.so
set environment NUAH_LIBROBLOX_ALLOCATOR_TLS_PATCH 1
set environment NUAH_PTHREAD_NAMESPACE 1
set environment NUAH_ANDROID_RUNTIME compat
catch signal SIGABRT
commands
 silent
 printf "SIGABRT pc=%p\\n", $pc
 bt 12
 info registers rbx r12 r13 r14 r15 rsp
 x/8i $pc
 quit
end
run native-run --width 1220 --height 980 --apk /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/base.apk --split /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/split_config.x86_64.apk --data /home/niggermonkey/.local/share/nuah --uri roblox://placeId=17625359962
