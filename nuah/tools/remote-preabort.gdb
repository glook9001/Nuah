set pagination off
set confirm off
set breakpoint pending on
set follow-fork-mode child
set detach-on-fork off
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
break pthread_key_create
commands
 silent
 set $ret = *(void **)$rsp
 printf "KEY pc=%p ret=%p low=%x\\n", $pc, $ret, (unsigned int)$ret & 0xfff
 if (((unsigned int)$ret & 0xfff) == 0x764)
   set $base = $ret - 0x2983764
   printf "SETTING BASE=%p\\n", $base
   break *($base + 0x2983700)
   commands
    silent
    printf "PREABORT pc=%p rbx=%p r12=%p r13=%p r14=%p r15=%p rdi=%p rsi=%p rdx=%p\\n", $pc, $rbx, $r12, $r13, $r14, $r15, $rdi, $rsi, $rdx
    x/16gx $rbx
    x/16i $pc-32
    quit
   end
   break *($base + 0x1ca6aa2)
   commands
    silent
    printf "ALLOC-ENTRY rdi=%p rsi=%p rdx=%p rcx=%p\\n", $rdi, $rsi, $rdx, $rcx
    continue
   end
   break *($base + 0x2983649)
   commands
    silent
    printf "ALLOC-RETURN rax=%p rbx=%p r14=%p\\n", $rax, $rbx, $r14
    continue
   end
   break *($base + 0x1ca797a)
   commands
    silent
    printf "FALLBACK-ENTRY rdi=%p rsi=%p rdx=%p rcx=%p\\n", $rdi, $rsi, $rdx, $rcx
    x/20gx $rdi
    printf "BUCKET=%p NODE=%p\\n", *(void **)($rdi + 0x168), *(void **)(*(void **)($rdi + 0x168) + 8)
    x/20gx *(void **)($rdi + 0x168)
    continue
   end
 end
 continue
end
run native-run --width 1220 --height 980 --apk /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/base.apk --split /home/niggermonkey/Desktop/nuah-rivals-current/nuah-rivals-turbo-1220x980/apk/split_config.x86_64.apk --data /home/niggermonkey/.local/share/nuah --uri roblox://placeId=17625359962
