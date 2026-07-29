cmake_minimum_required(VERSION 3.25)
project(nuah VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NUAH_ANDROID_API_LEVEL 36)

include(CTest)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NUAH_UI REQUIRED IMPORTED_TARGET gtk4 libadwaita-1 webkitgtk-6.0)
pkg_check_modules(NUAH_PLATFORM REQUIRED IMPORTED_TARGET sdl3 libffi)
pkg_check_modules(NUAH_GIO REQUIRED IMPORTED_TARGET gio-2.0)

option(NUAH_BUILD_ATL "Build the legacy ATL fork during this configure" OFF)
set(NUAH_SDL3_ROOT "" CACHE PATH
    "Private prefix containing a prebuilt SDL3 development package")

set(NUAH_ATL_FULL_SOURCE "${CMAKE_SOURCE_DIR}/third_party/android_translation_layer")
set(NUAH_ATL_FULL_BUILD "${CMAKE_BINARY_DIR}/atl-full")
set(NUAH_ATL_BOOTCLASSPATH "" CACHE STRING
    "Android boot class path passed to the ATL Meson build")
set(NUAH_ATL_JAVA_HOME "" CACHE PATH
    "JDK home passed to ATL when jni.pc is unavailable")
if(NUAH_BUILD_ATL)
  find_program(MESON_EXECUTABLE meson REQUIRED)
  find_program(NINJA_EXECUTABLE ninja REQUIRED)
  if(NOT NUAH_ATL_BOOTCLASSPATH)
    message(FATAL_ERROR
      "NUAH_BUILD_ATL requires NUAH_ATL_BOOTCLASSPATH")
  endif()
  set(NUAH_ATL_MESON_OPTIONS --buildtype=release --prefix=/usr/local)
  list(APPEND NUAH_ATL_MESON_OPTIONS
       "-Datl_bootclasspath=${NUAH_ATL_BOOTCLASSPATH}")
  if(NUAH_ATL_JAVA_HOME)
    list(APPEND NUAH_ATL_MESON_OPTIONS
         "-Datl_java_home=${NUAH_ATL_JAVA_HOME}")
  endif()
  if(NOT EXISTS "${NUAH_ATL_FULL_SOURCE}/meson.build")
    message(FATAL_ERROR "Pinned Android Translation Layer source is missing")
  endif()
  if(EXISTS "${NUAH_ATL_FULL_BUILD}/build.ninja")
    set(NUAH_ATL_MESON_RECONFIGURE --reconfigure)
  else()
    set(NUAH_ATL_MESON_RECONFIGURE)
  endif()
  execute_process(
    COMMAND "${MESON_EXECUTABLE}" setup ${NUAH_ATL_MESON_RECONFIGURE}
            "${NUAH_ATL_FULL_BUILD}" "${NUAH_ATL_FULL_SOURCE}"
            ${NUAH_ATL_MESON_OPTIONS}
    RESULT_VARIABLE NUAH_ATL_FULL_SETUP_RESULT
    COMMAND_ECHO STDOUT)
  if(NOT NUAH_ATL_FULL_SETUP_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not configure pinned Android Translation Layer")
  endif()
  add_custom_target(nuah_atl_runtime
    COMMAND "${MESON_EXECUTABLE}" compile -C "${NUAH_ATL_FULL_BUILD}" android-translation-layer
    BYPRODUCTS "${NUAH_ATL_FULL_BUILD}/android-translation-layer"
    COMMENT "Building legacy ATL fork (transitional only)"
    VERBATIM)
  add_custom_target(nuah_atl_api_jar
    COMMAND "${NINJA_EXECUTABLE}" -C "${NUAH_ATL_FULL_BUILD}" src/api-impl/hax.jar
    BYPRODUCTS "${NUAH_ATL_FULL_BUILD}/src/api-impl/hax.jar"
    COMMENT "Building transitional ATL API classes"
    VERBATIM)
else()
  add_custom_target(nuah_atl_runtime)
  add_custom_target(nuah_atl_api_jar)
endif()

add_library(nuah_host_bridge SHARED nuah/src/native_window_bridge.cpp)
target_include_directories(nuah_host_bridge PUBLIC nuah/include)
target_compile_options(nuah_host_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)

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

# Host-side Bionic compatibility symbols consumed by libhybris hooks.  This is
# intentionally a provider named libbionic.so, never a replacement host libc.
add_library(nuah_android_bionic SHARED nuah/android_shims/bionic.cpp)
set_target_properties(nuah_android_bionic PROPERTIES
  OUTPUT_NAME bionic
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android"
  BUILD_RPATH "$ORIGIN")
target_compile_definitions(nuah_android_bionic PRIVATE
  NUAH_ANDROID_API_LEVEL=${NUAH_ANDROID_API_LEVEL})
target_compile_options(nuah_android_bionic PRIVATE
  -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(nuah_android_bionic PRIVATE
  "-Wl,--version-script=${CMAKE_SOURCE_DIR}/nuah/android_shims/bionic.map")

add_library(nuah_atl_overlay SHARED nuah/atl_overlay/compat.cpp)
target_compile_options(nuah_atl_overlay PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(
  nuah_atl_overlay PROPERTIES
  OUTPUT_NAME nuah_atl_overlay)

# The Nuah JVM is a narrow adapter around the licensed android2gnulinux JNI
# core.  Its old linker/platform layers are deliberately not compiled.
add_library(nuah_jvm_core STATIC
  third_party/android2gnulinux/src/jvm/jvm.c
  third_party/android2gnulinux/src/wrapper/wrapper.c
  nuah/src/nuah_jvm_adapter.c)
target_include_directories(nuah_jvm_core PRIVATE
  nuah/include
  third_party/android2gnulinux/src)
target_compile_definitions(nuah_jvm_core PRIVATE _GNU_SOURCE)
target_link_libraries(nuah_jvm_core PUBLIC dl pthread)

add_library(nuah_core
  nuah/src/atl_backend.cpp
  nuah/src/jni_contract.cpp
  nuah/src/jni_manifest.cpp
  nuah/src/input_bridge.cpp
  nuah/src/native_session.cpp
  nuah/src/window_session.cpp
  nuah/src/native_runtime.cpp
  nuah/src/apk_loader.cpp
  nuah/src/launch_uri.cpp
  nuah/src/sober_cache.cpp
  nuah/src/protocol.cpp)
target_include_directories(nuah_core PUBLIC nuah/include)
target_compile_definitions(nuah_core PRIVATE _GNU_SOURCE)
target_compile_definitions(
  nuah_core PRIVATE
  NUAH_ATL_OVERLAY_PATH="$<TARGET_FILE:nuah_atl_overlay>")
target_compile_options(nuah_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(NUAH_SDL3_ROOT)
  target_include_directories(nuah_core PRIVATE "${NUAH_SDL3_ROOT}/usr/include")
  target_link_directories(nuah_core PUBLIC
    "${NUAH_SDL3_ROOT}/usr/lib/x86_64-linux-gnu")
endif()
target_link_libraries(
  nuah_core PUBLIC nuah_host_bridge nuah_libnativehelper_jni nuah_jvm_core dl ZLIB::ZLIB PkgConfig::NUAH_GIO
  PkgConfig::NUAH_PLATFORM)
add_dependencies(nuah_core nuah_atl_overlay)
add_dependencies(nuah_core nuah_android_bionic)

add_library(nuah_android_log SHARED nuah/android_shims/log.cpp)
set_target_properties(nuah_android_log PROPERTIES OUTPUT_NAME log LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android")
target_compile_options(nuah_android_log PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)

add_library(nuah_android_platform SHARED nuah/android_shims/platform.cpp)
set_target_properties(nuah_android_platform PROPERTIES OUTPUT_NAME android LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/android")
target_compile_options(nuah_android_platform PRIVATE -Wall -Wextra -Wpedantic -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(nuah_android_platform PRIVATE nuah/include)
target_link_libraries(nuah_android_platform PRIVATE nuah_host_bridge nuah_android_registry)
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
target_link_libraries(nuah_jvm_registration_test PRIVATE nuah_jvm_core)
add_test(NAME nuah-jvm-registration COMMAND nuah_jvm_registration_test)

add_executable(nuah nuah/src/main.cpp)
target_link_libraries(nuah PRIVATE nuah_core)
add_dependencies(nuah nuah_atl_runtime)
add_dependencies(nuah nuah_atl_api_jar)
add_dependencies(nuah nuah_android_abi_registry_test)

add_executable(nuah-services nuah/src/services_main.cpp)
target_link_libraries(nuah-services PRIVATE nuah_core PkgConfig::NUAH_UI)
add_dependencies(nuah-services nuah_atl_runtime)
add_dependencies(nuah-services nuah_atl_api_jar)

configure_file(nuah/share/applications/org.nuah.Nuah.desktop
               ${CMAKE_BINARY_DIR}/org.nuah.Nuah.desktop COPYONLY)

if(BUILD_TESTING AND NUAH_BUILD_ATL)
  find_program(NUAH_JAVAP_EXECUTABLE javap REQUIRED)
  add_test(NAME nuah-atl-input-abi
    COMMAND "${CMAKE_COMMAND}"
      "-DJAVAP=${NUAH_JAVAP_EXECUTABLE}"
      "-DATL_JAR=${NUAH_ATL_FULL_BUILD}/src/api-impl/hax.jar"
      -P "${CMAKE_SOURCE_DIR}/nuah/tests/atl_input_abi.cmake")
endif()
