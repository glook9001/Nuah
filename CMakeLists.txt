cmake_minimum_required(VERSION 3.25)
project(nuah VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NUAH_ANDROID_API_LEVEL 36)

add_compile_options(
  $<$<COMPILE_LANGUAGE:C,CXX>:-fno-plt>
  $<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64-v3>
)

include(CTest)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NUAH_UI REQUIRED IMPORTED_TARGET gtk4 libadwaita-1 webkitgtk-6.0)
pkg_check_modules(NUAH_PLATFORM REQUIRED IMPORTED_TARGET sdl3 libffi)
pkg_check_modules(NUAH_GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(NUAH_ARCHIVE REQUIRED IMPORTED_TARGET libarchive)

# Keep the hot Android pthread bridge beside Nuah instead of silently using
# the distro copy.  The upstream library is deliberately built from its own
# source file; Nuah does not duplicate the pthread ABI or its implementation.
# Our pinned copy tags initialized host objects, avoiding an expensive
# mincore(2) probe on every Android lock/unlock/wait operation.
add_library(nuah_pthread_bio SHARED
  third_party/bionic_translation/pthread_wrapper/libpthread.c)
set_target_properties(nuah_pthread_bio PROPERTIES
  OUTPUT_NAME pthread_bio
  SOVERSION 0
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bionic-translation")
target_compile_definitions(nuah_pthread_bio PRIVATE _GNU_SOURCE)
target_compile_options(nuah_pthread_bio PRIVATE -O3 -fPIC)
target_link_libraries(nuah_pthread_bio PRIVATE pthread dl)

set(NUAH_SDL3_ROOT "" CACHE PATH
    "Private prefix containing a prebuilt SDL3 development package")
if(NUAH_SDL3_ROOT)
  include_directories("${NUAH_SDL3_ROOT}/usr/include")
  link_directories("${NUAH_SDL3_ROOT}/usr/lib/x86_64-linux-gnu")
endif()

add_library(nuah_host_bridge SHARED nuah/src/native_window_bridge.cpp)
target_include_directories(nuah_host_bridge PUBLIC nuah/include)
target_compile_options(nuah_host_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_libraries(nuah_host_bridge PRIVATE dl)

add_library(nuah_bootstrap_diagnostics STATIC
  nuah/src/bootstrap_diagnostics.cpp)
target_include_directories(nuah_bootstrap_diagnostics PUBLIC nuah/include)
target_compile_options(nuah_bootstrap_diagnostics PRIVATE
  -Wall -Wextra -Wpedantic -Werror)

# Android's official JNI 1.6 headers.  libnativehelper's actual invocation
# library delegates to ART, which Nuah intentionally does not bundle.
add_library(nuah_libnativehelper_jni INTERFACE)
target_include_directories(nuah_libnativehelper_jni INTERFACE
  "${CMAKE_SOURCE_DIR}/third_party/libnativehelper/include_jni")

add_library(nuah_android_registry SHARED nuah/android_shims/registry.cpp)
set_target_properties(
  nuah_android_registry PROPERTIES
  OUTPUT_NAME nuah_android_registry
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android")
target_include_directories(nuah_android_registry PUBLIC nuah/include)
target_compile_options(nuah_android_registry PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
set_target_properties(nuah_android_registry PROPERTIES BUILD_RPATH "$ORIGIN")

# Upstream libhybris still expects a file for each DT_NEEDED Android soname.
# These tiny, dependency-free ELF records satisfy that lookup without
# modifying its linker or shipping an Android libc/APEX.  Actual symbols are
# resolved by the libhybris hook callback and Nuah's host-loaded providers.
set(NUAH_LINKER_DEPENDENCY_DIR "${CMAKE_BINARY_DIR}/android/linker-deps")
set(NUAH_LINKER_DEPENDENCY_NAMES
  libOpenMAXAL.so libmediandk.so libOpenSLES.so libGLESv2.so libEGL.so
  libandroid.so liblog.so libm.so libdl.so libc.so)
set(NUAH_LINKER_DEPENDENCY_OUTPUTS)
foreach(soname IN LISTS NUAH_LINKER_DEPENDENCY_NAMES)
  set(output "${NUAH_LINKER_DEPENDENCY_DIR}/${soname}")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${NUAH_LINKER_DEPENDENCY_DIR}"
    COMMAND "${CMAKE_C_COMPILER}" -shared -fPIC -nostdlib
            "-Wl,-soname,${soname}" "-Wl,--build-id=none"
            "${CMAKE_CURRENT_SOURCE_DIR}/nuah/android_shims/linker_stub.c"
            -o "${output}"
    DEPENDS nuah/android_shims/linker_stub.c
    VERBATIM)
  list(APPEND NUAH_LINKER_DEPENDENCY_OUTPUTS "${output}")
endforeach()
add_custom_target(nuah_android_linker_dependencies ALL
  DEPENDS ${NUAH_LINKER_DEPENDENCY_OUTPUTS})

# Host-side Bionic compatibility symbols consumed by libhybris hooks.  This is
# intentionally a provider named libbionic.so, never a replacement host libc.
add_library(nuah_android_bionic SHARED
  nuah/android_shims/bionic.cpp
  nuah/android_shims/bionic_forward.S
  nuah/android_shims/bionic_setjmp.S)
set_target_properties(nuah_android_bionic PROPERTIES
  OUTPUT_NAME bionic
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android"
  BUILD_RPATH "$ORIGIN")
target_compile_definitions(nuah_android_bionic PRIVATE
  NUAH_ANDROID_API_LEVEL=${NUAH_ANDROID_API_LEVEL})
target_compile_options(nuah_android_bionic PRIVATE
  -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(nuah_android_bionic PRIVATE nuah/include)
target_link_options(nuah_android_bionic PRIVATE
  "-Wl,--version-script=${CMAKE_SOURCE_DIR}/nuah/android_shims/bionic.map")

# The bundled Android linker resolves a few legacy fortify/property objects
# with dlsym(RTLD_DEFAULT), bypassing libhybris's per-relocation callback.
# Export only that finite helper ABI globally; do not expose the pthread/TLS
# wrappers from libbionic, which would create a second synchronization domain.
add_library(nuah_android_bionic_linker_helpers SHARED
  nuah/android_shims/bionic_linker_helpers.cpp)
set_target_properties(nuah_android_bionic_linker_helpers PROPERTIES
  OUTPUT_NAME bionic-linker-helpers
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android"
  BUILD_RPATH "$ORIGIN")
target_compile_definitions(nuah_android_bionic_linker_helpers PRIVATE
  NUAH_ANDROID_API_LEVEL=${NUAH_ANDROID_API_LEVEL})
target_compile_options(nuah_android_bionic_linker_helpers PRIVATE
  -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(nuah_android_bionic_linker_helpers PRIVATE nuah/include)
target_link_options(nuah_android_bionic_linker_helpers PRIVATE
  "-Wl,--version-script=${CMAKE_SOURCE_DIR}/nuah/android_shims/bionic-linker-helpers.map")

# The Nuah JVM is a narrow adapter around the licensed android2gnulinux JNI
# core.  Its old linker/platform layers are deliberately not compiled.
add_library(nuah_jvm_core STATIC
  third_party/android2gnulinux/src/jvm/jvm.c
  third_party/android2gnulinux/src/libjvm-java.c
  third_party/android2gnulinux/src/wrapper/wrapper.c
  nuah/src/roblox_java_facade.c
  nuah/src/nuah_jvm_adapter.c)
target_include_directories(nuah_jvm_core PRIVATE
  nuah/include
  third_party/android2gnulinux/src)
target_compile_definitions(nuah_jvm_core PRIVATE _GNU_SOURCE)
target_link_libraries(nuah_jvm_core PUBLIC dl pthread)

add_library(nuah_core
  nuah/src/native_backend.cpp
  nuah/src/jni_contract.cpp
  nuah/src/jni_manifest.cpp
  nuah/src/input_bridge.cpp
  nuah/src/perf_metrics.cpp
  nuah/src/native_session.cpp
  nuah/src/window_session.cpp
  nuah/src/real_art_jvm.cpp
  nuah/src/native_runtime.cpp
  nuah/src/apk_loader.cpp
  nuah/src/launch_uri.cpp
  nuah/src/sober_cache.cpp
  nuah/src/protocol.cpp)
target_include_directories(nuah_core PUBLIC nuah/include)
target_compile_definitions(nuah_core PRIVATE _GNU_SOURCE)
target_compile_options(nuah_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NUAH_SDL3_ROOT)
  target_include_directories(nuah_core PRIVATE "${NUAH_SDL3_ROOT}/usr/include")
  target_link_directories(nuah_core PUBLIC
    "${NUAH_SDL3_ROOT}/usr/lib/x86_64-linux-gnu")
endif()
target_link_libraries(
  nuah_core PUBLIC nuah_host_bridge nuah_bootstrap_diagnostics nuah_libnativehelper_jni nuah_jvm_core dl ZLIB::ZLIB
  PkgConfig::NUAH_PLATFORM PkgConfig::NUAH_GLIB)
add_dependencies(nuah_core nuah_android_bionic)
add_dependencies(nuah_core nuah_android_bionic_linker_helpers)
add_dependencies(nuah_core nuah_android_math)
add_dependencies(nuah_core nuah_android_linker_dependencies)

add_library(nuah_android_log SHARED nuah/android_shims/log.cpp)
set_target_properties(nuah_android_log PROPERTIES OUTPUT_NAME log LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android")
target_compile_options(nuah_android_log PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(nuah_android_log PRIVATE nuah/include)

add_library(nuah_android_math SHARED nuah/android_shims/math.cpp)
set_target_properties(nuah_android_math PROPERTIES
  OUTPUT_NAME m
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android"
  BUILD_RPATH "$ORIGIN")
target_include_directories(nuah_android_math PRIVATE nuah/include)
target_compile_options(nuah_android_math PRIVATE
  -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(nuah_android_math PRIVATE
  "-Wl,--version-script=${CMAKE_SOURCE_DIR}/nuah/android_shims/math.map")
target_link_libraries(nuah_android_math PRIVATE
  m nuah_android_registry)

add_library(nuah_android_platform SHARED
  nuah/android_shims/asset_manager.cpp
  nuah/android_shims/egl.cpp
  nuah/android_shims/looper_fast.cpp
  nuah/android_shims/platform.cpp)
set_target_properties(nuah_android_platform PROPERTIES OUTPUT_NAME android LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android")
target_compile_options(nuah_android_platform PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_compile_definitions(nuah_android_platform PRIVATE _LARGEFILE64_SOURCE)
target_include_directories(nuah_android_platform PRIVATE nuah/include)
if(NUAH_SDL3_ROOT)
  target_include_directories(nuah_android_platform PRIVATE
    "${NUAH_SDL3_ROOT}/usr/include")
endif()
target_link_libraries(nuah_android_platform PRIVATE
  nuah_host_bridge nuah_android_registry PkgConfig::NUAH_ARCHIVE
  PkgConfig::NUAH_PLATFORM dl)
set_target_properties(nuah_android_platform PROPERTIES BUILD_RPATH "$ORIGIN:$ORIGIN/..")

add_library(nuah_android_vulkan SHARED nuah/android_shims/vulkan.cpp)
set_target_properties(
  nuah_android_vulkan PROPERTIES
  OUTPUT_NAME vulkan
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android"
  BUILD_RPATH "$ORIGIN:$ORIGIN/..")
target_include_directories(nuah_android_vulkan PRIVATE nuah/include)
target_compile_options(nuah_android_vulkan PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
if(NUAH_SDL3_ROOT)
  target_include_directories(nuah_android_vulkan PRIVATE
    "${NUAH_SDL3_ROOT}/usr/include")
  target_link_directories(nuah_android_vulkan PRIVATE
    "${NUAH_SDL3_ROOT}/usr/lib/x86_64-linux-gnu")
endif()
target_link_libraries(
  nuah_android_vulkan PRIVATE dl PkgConfig::NUAH_PLATFORM nuah_host_bridge
  nuah_android_registry)
add_library(nuah_android_media SHARED nuah/android_shims/platform.cpp)
set_target_properties(nuah_android_media PROPERTIES OUTPUT_NAME mediandk LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android")
target_compile_options(nuah_android_media PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(nuah_android_media PRIVATE nuah/include)
target_link_libraries(nuah_android_media PRIVATE nuah_host_bridge nuah_android_registry)
set_target_properties(nuah_android_media PROPERTIES BUILD_RPATH "$ORIGIN:$ORIGIN/..")
add_library(nuah_android_opensles SHARED nuah/android_shims/platform.cpp)
set_target_properties(nuah_android_opensles PROPERTIES OUTPUT_NAME OpenSLES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android")
target_compile_options(nuah_android_opensles PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(nuah_android_opensles PRIVATE nuah/include)
target_link_libraries(nuah_android_opensles PRIVATE nuah_host_bridge nuah_android_registry)
set_target_properties(nuah_android_opensles PROPERTIES BUILD_RPATH "$ORIGIN:$ORIGIN/..")
add_library(nuah_android_openmaxal SHARED nuah/android_shims/platform.cpp)
set_target_properties(nuah_android_openmaxal PROPERTIES OUTPUT_NAME OpenMAXAL LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android")
target_compile_options(nuah_android_openmaxal PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(nuah_android_openmaxal PRIVATE nuah/include)
target_link_libraries(nuah_android_openmaxal PRIVATE nuah_host_bridge nuah_android_registry)
set_target_properties(nuah_android_openmaxal PROPERTIES BUILD_RPATH "$ORIGIN:$ORIGIN/..")

add_executable(nuah_android_abi_registry_test
  nuah/tests/android_abi_registry_test.cpp)
target_include_directories(nuah_android_abi_registry_test PRIVATE nuah/include)
target_compile_options(nuah_android_abi_registry_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nuah_android_abi_registry_test PRIVATE nuah_android_registry)
set_target_properties(nuah_android_abi_registry_test PROPERTIES BUILD_RPATH
  "$ORIGIN/android")
add_test(NAME nuah-android-abi-registry
  COMMAND nuah_android_abi_registry_test)

add_executable(nuah_native_contract_test nuah/tests/native_contract_test.cpp)
target_include_directories(nuah_native_contract_test PRIVATE nuah/include)
if(NUAH_SDL3_ROOT)
  target_include_directories(nuah_native_contract_test PRIVATE
    "${NUAH_SDL3_ROOT}/usr/include")
endif()
target_compile_options(nuah_native_contract_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nuah_native_contract_test PRIVATE nuah_core)
add_test(NAME nuah-native-contract COMMAND nuah_native_contract_test)

add_executable(nuah_jni_manifest_test nuah/tests/jni_manifest_test.cpp)
target_include_directories(nuah_jni_manifest_test PRIVATE nuah/include)
target_compile_options(nuah_jni_manifest_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(nuah_jni_manifest_test PRIVATE
  NUAH_JNI_MANIFEST_PATH="${CMAKE_SOURCE_DIR}/nuah/share/roblox-jni-contract.tsv")
target_link_libraries(nuah_jni_manifest_test PRIVATE nuah_core)
add_test(NAME nuah-jni-manifest COMMAND nuah_jni_manifest_test)

add_executable(nuah_jvm_registration_test nuah/tests/nuah_jvm_registration_test.c)
target_include_directories(nuah_jvm_registration_test PRIVATE
  nuah/include
  third_party/android2gnulinux/src)
target_compile_options(nuah_jvm_registration_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nuah_jvm_registration_test PRIVATE nuah_jvm_core nuah_host_bridge)
set_target_properties(nuah_jvm_registration_test PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME nuah-jvm-registration COMMAND nuah_jvm_registration_test)

add_executable(nuah_bootstrap_diagnostics_test
  nuah/tests/bootstrap_diagnostics_test.cpp)
target_include_directories(nuah_bootstrap_diagnostics_test PRIVATE nuah/include)
target_compile_options(nuah_bootstrap_diagnostics_test PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(nuah_bootstrap_diagnostics_test PRIVATE
  NUAH_TEST_BIONIC_PATH="$<TARGET_FILE:nuah_android_bionic>")
target_link_libraries(nuah_bootstrap_diagnostics_test PRIVATE
  nuah_bootstrap_diagnostics dl)
add_dependencies(nuah_bootstrap_diagnostics_test nuah_android_bionic)
add_test(NAME nuah-bootstrap-diagnostics
  COMMAND nuah_bootstrap_diagnostics_test)

add_executable(nuah_bionic_sync_stress_test
  nuah/tests/bionic_sync_stress_test.cpp)
target_compile_options(nuah_bionic_sync_stress_test PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(nuah_bionic_sync_stress_test PRIVATE
  NUAH_TEST_BIONIC_PATH="$<TARGET_FILE:nuah_pthread_bio>")
target_link_libraries(nuah_bionic_sync_stress_test PRIVATE dl)
add_dependencies(nuah_bionic_sync_stress_test nuah_pthread_bio)
add_test(NAME nuah-bionic-sync-stress
  COMMAND nuah_bionic_sync_stress_test)

add_executable(nuah nuah/src/main.cpp)
target_link_libraries(nuah PRIVATE nuah_core)
set_target_properties(nuah PROPERTIES BUILD_RPATH "$ORIGIN" ENABLE_EXPORTS ON)
add_dependencies(nuah nuah_android_abi_registry_test)
add_dependencies(nuah nuah_pthread_bio)

add_executable(nuah-services nuah/src/services_main.cpp)
target_link_libraries(nuah-services PRIVATE nuah_core PkgConfig::NUAH_UI)
set_target_properties(nuah-services PROPERTIES BUILD_RPATH "$ORIGIN")
