#include "nuah/nuah_jvm.h"
#include "nuah/input_bridge.h"
#include "nuah/native_window_bridge.h"

#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <filesystem>
#include <pthread.h>
#include <string>
#include <vector>

using NativeDlsym = void* (*)(void*, const char*);
using NativeDlopen = void* (*)(const char*, int);

struct NuahJvm {
  JavaVM* vm = nullptr;
  JNIEnv* env = nullptr;
  /* JNI_CreateJavaVM attaches the calling pthread as ART's "main" thread.
   * Nuah runs that call on the isolated bootstrap worker, so it must detach
   * before the worker returns.  Otherwise ART reports the thread exit and
   * repeatedly enters ScopedThreadStateChange checks during teardown. */
  pthread_t vm_owner{};
  bool vm_owner_attached = false;
  void* art = nullptr;
  void* api_native = nullptr;
  // ATL's provider must be looked up through the same linker that ART uses.
  // A glibc dlsym on a libhybris/bionic handle can resolve a second copy of
  // the provider and duplicate its GTK GObject types.
  NativeDlsym api_dlsym = nullptr;
  jobject activity = nullptr;
  jobject surface = nullptr;
  jobject key_event = nullptr;
  jobject motion_event = nullptr;
  jclass native_input_interface = nullptr;
  jmethodID native_pass_mouse_move = nullptr;
  jmethodID native_pass_mouse_button = nullptr;
  jmethodID native_pass_mouse_wheel = nullptr;
  jmethodID native_get_mouse_locked_center = nullptr;
  bool native_input_lookup_attempted = false;
  jclass native_gl_interface = nullptr;
  jmethodID native_pass_key_event = nullptr;
  bool native_gl_lookup_attempted = false;
  jlong native_handle = 0;
  NuahNativeWindow* surface_window = nullptr;
  std::string class_path;
};

extern "C" {
void* window;
char* apk_path;
// ATL's libandroid.so.0 references the executable-owned MessageQueue thread
// slot.  The translation-layer JNI library initializes this slot when its
// queue is created; exporting the same storage lets the two real ATL DSOs
// resolve their circular dependency without a fake implementation.
void* main_thread_id = nullptr;
}

extern "C" char* get_app_data_dir() {
  static std::string path;
  if (path.empty()) {
    if (const char* value = std::getenv("ANDROID_APP_DATA_DIR"); value && *value)
      path = value;
    else
      path = "/tmp/nuah";
  }
  return path.data();
}

extern "C" void back_button_set_sensitive(bool) {}

extern "C" jint nuah_art_log_println(JNIEnv* env, jclass, jint, jint,
                                      jstring, jstring message) {
  static const bool android_log_enabled = [] {
    const char* v = std::getenv("NUAH_ANDROID_LOG");
    return v && *v && std::strcmp(v, "0") != 0;
  }();
  if (android_log_enabled && env && message) {
    const char* text = env->GetStringUTFChars(message, nullptr);
    if (text) {
      std::fprintf(stderr, "[android] %s\n", text);
      env->ReleaseStringUTFChars(message, text);
    }
  }
  return 0;
}

extern "C" void nuah_art_logging_protocol_native_log_event(
    JNIEnv*, jclass, jstring, jlong, jobjectArray) {}

extern "C" void nuah_art_context_update_config(JNIEnv*, jclass, jobject) {
  // SDL owns the host window; ATL's GTK monitor callback is not valid in this
  // process.  Roblox only needs Context's static configuration bootstrap to
  // complete here; the dimensions are supplied by Nuah's Surface bridge.
}

namespace {

using CreateJavaVm = jint (*)(JavaVM**, void**, void*);
using AtlJniSetup = void (*)(JNIEnv*);
using GtkInitCheck = int (*)();

bool trace_enabled();

bool is_gameplay_key(int keycode) {
  /* Roblox's desktop input layer consumes these through
   * NativeGLInterface.nativePassKeyEvent.  MainGameActivity.onKeyDown can
   * report handled without forwarding them when the Android View hierarchy
   * is only a façade, so do not use that return value as proof that Roblox
   * received the key. */
  return (keycode >= NUAH_KEY_0 && keycode <= NUAH_KEY_9) ||
         (keycode >= NUAH_KEY_DPAD_UP && keycode <= NUAH_KEY_DPAD_RIGHT) ||
         (keycode >= NUAH_KEY_A && keycode <= NUAH_KEY_Z) ||
         keycode == NUAH_KEY_TAB || keycode == NUAH_KEY_SPACE ||
         keycode == NUAH_KEY_ENTER || keycode == NUAH_KEY_SHIFT_LEFT ||
         keycode == NUAH_KEY_SHIFT_RIGHT || keycode == NUAH_KEY_CTRL_LEFT ||
         keycode == NUAH_KEY_CTRL_RIGHT || keycode == NUAH_KEY_ALT_LEFT ||
         keycode == NUAH_KEY_ALT_RIGHT || keycode == NUAH_KEY_ESCAPE;
}

void* api_symbol(const NuahJvm* jvm, const char* symbol) {
  if (!jvm || !jvm->api_native || !symbol) return nullptr;
  return jvm->api_dlsym ? jvm->api_dlsym(jvm->api_native, symbol)
                        : ::dlsym(jvm->api_native, symbol);
}

void initialize_atl_host_state(NuahJvm* jvm) {
  if (!jvm || !jvm->env || !jvm->api_native) return;

  // The standalone ATL executable performs these steps after creating ART.
  // Nuah loads ATL as a provider, so its main executable is not present to do
  // them.  Without the cache, ATL's widget callbacks compare/use uninitialized
  // jmethodIDs; without its JavaVM global, deferred GTK callbacks use an
  // invalid JNI environment.
  if (auto* vm_slot = reinterpret_cast<JavaVM**>(
          api_symbol(jvm, "jvm"))) {
    *vm_slot = jvm->vm;
  }
  if (auto setup = reinterpret_cast<AtlJniSetup>(
          api_symbol(jvm, "set_up_handle_cache"))) {
    setup(jvm->env);
  } else if (trace_enabled()) {
    std::fprintf(stderr, "nuah ART: ATL handle-cache setup export missing\n");
  }

  // ATL's widget JNI uses GTK types directly.  Its normal executable calls
  // GTK initialization before constructing the first Activity; mirror only
  // that host initialization here.  Nuah still owns the SDL/Vulkan window.
  auto gtk_init = reinterpret_cast<GtkInitCheck>(
      ::dlsym(RTLD_DEFAULT, "gtk_init_check"));
  if (gtk_init && !gtk_init()) {
    if (trace_enabled())
      std::fprintf(stderr, "nuah ART: GTK display initialization failed\n");
  }
}

bool trace_enabled() {
  const char* value = std::getenv("NUAH_BOOTSTRAP_TRACE");
  return value && *value && std::strcmp(value, "0") != 0;
}

bool input_trace_enabled() {
  const char* value = std::getenv("NUAH_INPUT_TRACE");
  return value && *value && std::strcmp(value, "0") != 0;
}

void clear_exception(JNIEnv* env, const char* boundary) {
  if (!env || !env->ExceptionCheck()) return;
  if (trace_enabled()) {
    std::fprintf(stderr, "nuah ART: Java exception at %s\n", boundary);
    env->ExceptionDescribe();
  }
  env->ExceptionClear();
}

jclass find_class(JNIEnv* env, const char* name) {
  if (!env || !name) return nullptr;
  jclass klass = env->FindClass(name);
  if (!klass) clear_exception(env, name);
  return klass;
}

std::string artifact_root() {
  if (const char* value = std::getenv("NUAH_ART_HOME"); value && *value)
    return value;
  if (const char* value = std::getenv("NUAH_ATL_HOME"); value && *value)
    return value;
  return "/usr/local/lib64/java/dex/android_translation_layer";
}

std::string art_path() {
  if (const char* value = std::getenv("NUAH_ART_LIBRARY"); value && *value)
    return value;
  if (const char* directory = std::getenv("NUAH_ART_LIBRARY_DIR");
      directory && *directory)
    return (std::filesystem::path(directory) / "libart.so").string();
  return "/usr/local/lib64/art/libart.so";
}

std::vector<std::string> apk_class_paths() {
  std::vector<std::string> paths;
  const char* value = std::getenv("NUAH_APK_PATHS");
  if (!value || !*value) return paths;
  std::string all(value);
  std::size_t begin = 0;
  while (begin <= all.size()) {
    const std::size_t end = all.find(':', begin);
    const std::string path = all.substr(begin, end == std::string::npos
                                                  ? std::string::npos
                                                  : end - begin);
    if (!path.empty() && std::filesystem::is_regular_file(path))
      paths.push_back(path);
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return paths;
}

std::string join(const std::vector<std::string>& values) {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) result.push_back(':');
    result += value;
  }
  return result;
}

/* The host ART image is tied to the exact boot-classpath order used when it
 * was compiled.  Native-run used to leave the inherited (and often stale)
 * BOOTCLASSPATH in place, so ART rejected boot.art and retried every app dex
 * file in imageless mode.  Keep this small normalization local to the real
 * VM path. */
std::string android16_bootclasspath() {
  static constexpr const char* kOrder[] = {
      "core-oj-hostdex.jar",       "apachehttp-hostdex.jar",
      "apache-xml-hostdex.jar",    "bouncycastle-hostdex.jar",
      "core-junit-hostdex.jar",    "core-libart-hostdex.jar",
      "hamcrest-hostdex.jar",      "junit-runner-hostdex.jar",
      "okhttp-hostdex.jar",        "wolfssljni-hostdex.jar",
  };

  std::vector<std::filesystem::path> entries;
  const char* configured = std::getenv("NUAH_ATL_ANDROID16_BOOTCLASSPATH");
  if (configured && *configured) {
    std::string value(configured);
    std::size_t begin = 0;
    while (begin <= value.size()) {
      const std::size_t end = value.find(':', begin);
      std::filesystem::path path = value.substr(
          begin, end == std::string::npos ? std::string::npos : end - begin);
      if (!path.empty()) {
        if (path.is_relative()) {
          if (const char* home = std::getenv("NUAH_ATL_ANDROID16_HOME");
              home && *home)
            path = std::filesystem::path(home) / path;
        }
        entries.push_back(std::move(path));
      }
      if (end == std::string::npos) break;
      begin = end + 1;
    }
  }
  if (entries.empty()) {
    const std::filesystem::path root =
        "/usr/local/lib64/java/dex/art";
    for (const char* basename : kOrder) entries.emplace_back(root / basename);
  }

  std::vector<std::filesystem::path> ordered;
  std::vector<bool> used(entries.size(), false);
  ordered.reserve(entries.size());
  for (const char* basename : kOrder) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
      if (!used[index] && entries[index].filename() == basename) {
        ordered.push_back(entries[index]);
        used[index] = true;
      }
    }
  }
  for (std::size_t index = 0; index < entries.size(); ++index)
    if (!used[index]) ordered.push_back(entries[index]);

  std::vector<std::string> existing;
  existing.reserve(ordered.size());
  for (const auto& path : ordered) {
    if (!std::filesystem::is_regular_file(path)) return {};
    existing.push_back(path.string());
  }
  return join(existing);
}

void delete_global(JNIEnv* env, jobject& object) {
  if (env && object) env->DeleteGlobalRef(object);
  object = nullptr;
}

void delete_global_class(JNIEnv* env, jclass& klass) {
  if (env && klass) env->DeleteGlobalRef(klass);
  klass = nullptr;
}

jmethodID find_static_method(JNIEnv* env, jclass klass, const char* name,
                             const char* signature) {
  if (!env || !klass) return nullptr;
  jmethodID method = env->GetStaticMethodID(klass, name, signature);
  if (!method) clear_exception(env, name);
  return method;
}

bool ensure_native_input_methods(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return false;
  if (jvm->native_input_lookup_attempted)
    return jvm->native_input_interface != nullptr;
  jvm->native_input_lookup_attempted = true;
  jclass local = find_class(
      jvm->env, "com/roblox/engine/jni/NativeInputInterface");
  if (!local) return false;
  jvm->native_input_interface = static_cast<jclass>(
      jvm->env->NewGlobalRef(local));
  jvm->env->DeleteLocalRef(local);
  if (!jvm->native_input_interface) return false;
  jvm->native_pass_mouse_move = find_static_method(
      jvm->env, jvm->native_input_interface, "nativePassMouseMove",
      "(FFFF)V");
  jvm->native_pass_mouse_button = find_static_method(
      jvm->env, jvm->native_input_interface, "nativePassMouseButton",
      "(FFZI)V");
  jvm->native_pass_mouse_wheel = find_static_method(
      jvm->env, jvm->native_input_interface, "nativePassMouseWheel",
      "(FFF)V");
  jvm->native_get_mouse_locked_center = find_static_method(
      jvm->env, jvm->native_input_interface,
      "nativeGetMainWindowIsMouseLockedCenter", "()Z");
  if (input_trace_enabled()) {
    std::fprintf(stderr,
                 "nuah input: NativeInputInterface move=%p button=%p "
                 "wheel=%p locked=%p\n",
                 reinterpret_cast<void*>(jvm->native_pass_mouse_move),
                 reinterpret_cast<void*>(jvm->native_pass_mouse_button),
                 reinterpret_cast<void*>(jvm->native_pass_mouse_wheel),
                 reinterpret_cast<void*>(jvm->native_get_mouse_locked_center));
  }
  return jvm->native_pass_mouse_move || jvm->native_pass_mouse_button ||
         jvm->native_pass_mouse_wheel;
}

bool ensure_native_gl_method(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return false;
  if (jvm->native_gl_lookup_attempted)
    return jvm->native_gl_interface != nullptr &&
           jvm->native_pass_key_event != nullptr;
  jvm->native_gl_lookup_attempted = true;
  jclass local = find_class(
      jvm->env, "com/roblox/engine/jni/NativeGLInterface");
  if (!local) return false;
  jvm->native_gl_interface = static_cast<jclass>(
      jvm->env->NewGlobalRef(local));
  jvm->env->DeleteLocalRef(local);
  if (!jvm->native_gl_interface) return false;
  jvm->native_pass_key_event = find_static_method(
      jvm->env, jvm->native_gl_interface, "nativePassKeyEvent", "(ZIIZ)V");
  return jvm->native_pass_key_event != nullptr;
}

jobject make_empty(JNIEnv* env, const char* class_name) {
  jclass klass = find_class(env, class_name);
  if (!klass) return nullptr;
  jmethodID ctor = env->GetMethodID(klass, "<init>", "()V");
  if (!ctor) {
    clear_exception(env, class_name);
    return env->AllocObject(klass);
  }
  jobject result = env->NewObject(klass, ctor);
  clear_exception(env, class_name);
  return result;
}

jobject new_global_event(JNIEnv* env, jobject& slot, jobject local) {
  delete_global(env, slot);
  if (!local) return nullptr;
  slot = env->NewGlobalRef(local);
  return slot;
}

jmethodID find_instance_method(JNIEnv* env, const char* klass_name,
                               const char* method, const char* signature) {
  jclass klass = find_class(env, klass_name);
  if (!klass) return nullptr;
  jmethodID result = env->GetMethodID(klass, method, signature);
  if (!result) clear_exception(env, method);
  return result;
}

bool register_host_native(JNIEnv* env, const char* klass_name,
                          const char* method_name, const char* signature,
                          void* function) {
  jclass klass = find_class(env, klass_name);
  if (!klass || !function) return false;
  const JNINativeMethod method{method_name, signature, function};
  const jint result = env->RegisterNatives(klass, &method, 1);
  if (result != JNI_OK) clear_exception(env, method_name);
  return result == JNI_OK;
}

struct AtlNativeSpec {
  const char* klass;
  const char* method;
  const char* signature;
  const char* symbol;
};

void register_atl_natives(JNIEnv* env, void* handle, NativeDlsym lookup,
                          const AtlNativeSpec* specs, std::size_t count) {
  if (!env || !handle || !specs) return;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& spec = specs[index];
    void* function = lookup ? lookup(handle, spec.symbol)
                            : dlsym(handle, spec.symbol);
    if (!function && trace_enabled()) {
      std::fprintf(stderr, "nuah ART: ATL export missing %s\n", spec.symbol);
    }
    (void)register_host_native(env, spec.klass, spec.method, spec.signature,
                               function);
  }
}

bool load_atl_library_for_classloader(JNIEnv* env) {
  if (!env) return false;
  // This is the exact handoff used by ATL's launcher. A plain dlopen makes
  // the symbols visible to Nuah, but ART's native-method resolver only
  // searches libraries associated with the class loader. Register the same
  // provider with android.view.View's loader so every generated ATL JNI
  // export is discoverable without a second hand-written registry.
  jclass view = find_class(env, "android/view/View");
  jclass runtime_class = find_class(env, "java/lang/Runtime");
  if (!view || !runtime_class) return false;
  const jmethodID get_loader =
      env->GetMethodID(env->GetObjectClass(view), "getClassLoader",
                       "()Ljava/lang/ClassLoader;");
  const jmethodID get_runtime = env->GetStaticMethodID(
      runtime_class, "getRuntime", "()Ljava/lang/Runtime;");
  const jmethodID load_library = env->GetMethodID(
      runtime_class, "loadLibrary",
      "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
  if (!get_loader || !get_runtime || !load_library) {
    clear_exception(env, "ATL Runtime.loadLibrary lookup");
    return false;
  }
  jobject loader = env->CallObjectMethod(view, get_loader);
  jobject runtime = env->CallStaticObjectMethod(runtime_class, get_runtime);
  jstring name = env->NewStringUTF("translation_layer_main");
  env->CallVoidMethod(runtime, load_library, name, loader);
  env->DeleteLocalRef(name);
  if (env->ExceptionCheck()) {
    if (trace_enabled())
      std::fprintf(stderr, "nuah ART: ATL Runtime.loadLibrary failed\n");
    clear_exception(env, "ATL Runtime.loadLibrary");
    return false;
  }
  return true;
}

bool initialize_roblox_device_static_params(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return false;
  JNIEnv* env = jvm->env;
  jclass bridge = find_class(
      env, "com/roblox/engine/jni/NativeGLJavaInterface");
  jclass params_class = find_class(
      env, "com/roblox/engine/jni/model/DeviceStaticParams");
  if (!bridge || !params_class) return false;
  const jmethodID ctor = env->GetMethodID(params_class, "<init>", "()V");
  const jmethodID setter = env->GetStaticMethodID(
      bridge, "setDeviceStaticParams",
      "(Lcom/roblox/engine/jni/model/DeviceStaticParams;)V");
  if (!ctor || !setter) {
    clear_exception(env, "DeviceStaticParams setup methods");
    return false;
  }
  jobject params = env->NewObject(params_class, ctor);
  if (!params) {
    clear_exception(env, "DeviceStaticParams setup object");
    return false;
  }
  auto set_string = [&](const char* field_name, const char* value) {
    const jfieldID field = env->GetFieldID(
        params_class, field_name, "Ljava/lang/String;");
    if (!field) return false;
    const jstring text = env->NewStringUTF(value);
    env->SetObjectField(params, field, text);
    env->DeleteLocalRef(text);
    return !env->ExceptionCheck();
  };
  const jfieldID cpu = env->GetFieldID(params_class, "cpu64Bit", "Z");
  const bool ok =
      cpu && set_string("appBuildVariant", "release") &&
      set_string("appVersion", "Roblox") &&
      set_string("deviceName", "Nuah Linux PC") &&
      set_string("deviceSku", "x86_64") &&
      set_string("manufacturer", "Nuah") &&
      set_string("osVersion", "36") &&
      set_string("socModel", "x86_64");
  if (!ok || env->ExceptionCheck()) {
    clear_exception(env, "DeviceStaticParams setup fields");
    return false;
  }
  env->SetBooleanField(params, cpu, JNI_TRUE);
  env->CallStaticVoidMethod(bridge, setter, params);
  if (env->ExceptionCheck()) {
    clear_exception(env, "DeviceStaticParams setup setter");
    return false;
  }
  return true;
}

// ATL's framework reports the Android API level through Build.VERSION.  ART
// accepts the -D option below on some host builds, but not all of the bundled
// host ART variants propagate it before the framework classes are initialized.
// Set the property at the VM boundary and repair the two public version fields
// if Build.VERSION was initialized early.  This keeps the API-level contract in
// one place instead of adding version checks throughout the graphics bridge.
void force_android_sdk_level(JNIEnv* env, jint sdk) {
  if (!env) return;

  jclass system = find_class(env, "java/lang/System");
  if (system) {
    const jmethodID set_property = env->GetStaticMethodID(
        system, "setProperty",
        "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (set_property) {
      const jstring key = env->NewStringUTF("Build.VERSION.SDK_INT");
      const jstring value = env->NewStringUTF("36");
      env->CallStaticObjectMethod(system, set_property, key, value);
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(value);
      clear_exception(env, "System.setProperty(Build.VERSION.SDK_INT)");
    } else {
      clear_exception(env, "System.setProperty lookup");
    }
  }

  jclass version = find_class(env, "android/os/Build$VERSION");
  if (!version) return;
  const jfieldID sdk_field = env->GetStaticFieldID(version, "SDK_INT", "I");
  if (sdk_field) env->SetStaticIntField(version, sdk_field, sdk);
  clear_exception(env, "Build.VERSION.SDK_INT");
  const jfieldID resources_field =
      env->GetStaticFieldID(version, "RESOURCES_SDK_INT", "I");
  if (resources_field) env->SetStaticIntField(version, resources_field, sdk);
  clear_exception(env, "Build.VERSION.RESOURCES_SDK_INT");

  const jfieldID sdk_string =
      env->GetStaticFieldID(version, "SDK", "Ljava/lang/String;");
  if (sdk_string) {
    const jstring value = env->NewStringUTF("36");
    env->SetStaticObjectField(version, sdk_string, value);
    env->DeleteLocalRef(value);
  }
  clear_exception(env, "Build.VERSION.SDK");

  if (trace_enabled()) {
    std::fprintf(stderr, "nuah ART: forced Android SDK level %d\n",
                 static_cast<int>(sdk));
  }
}

void bind_host_r_debug() {
  /* ART's libart is DT_NEEDED on libdl_bio. That linker stores a host
   * r_debug pointer; leaving it null makes apkenv_find_library fault at
   * offset 0x18 on the first bionic_dlopen during JNI_CreateJavaVM. */
  void* bio = ::dlopen("libdl_bio.so.0", RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
  if (!bio) bio = ::dlopen("libdl_bio.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!bio) return;
  auto** linker_debug = reinterpret_cast<struct r_debug**>(
      ::dlsym(bio, "_r_debug_ptr"));
  if (linker_debug && !*linker_debug) {
    if (auto* host = reinterpret_cast<struct r_debug*>(
            ::dlsym(RTLD_DEFAULT, "_r_debug"))) {
      *linker_debug = host;
    }
  }
}

}  // namespace

extern "C" NuahJvm* nuah_jvm_create(void) {
  auto* jvm = new NuahJvm;
  jvm->art = dlopen(art_path().c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!jvm->art) {
    std::fprintf(stderr, "nuah ART: cannot load %s: %s\n", art_path().c_str(),
                 dlerror());
    delete jvm;
    return nullptr;
  }
  bind_host_r_debug();
  auto create = reinterpret_cast<CreateJavaVm>(dlsym(jvm->art, "JNI_CreateJavaVM"));
  if (!create) {
    std::fprintf(stderr, "nuah ART: libart has no JNI_CreateJavaVM\n");
    delete jvm;
    return nullptr;
  }

  const std::string root = artifact_root();
  const std::string api_jar = root + "/api-impl.jar";
  std::string framework = root + "/framework-res.apk";
  /* Keep framework-res on its verified install path when a copied or
   * symlinked archive would be rejected by the Android zip reader. */
  if (const char* configured = std::getenv("NUAH_ART_FRAMEWORK_RES");
      configured && *configured &&
      std::filesystem::is_regular_file(configured)) {
    framework = configured;
  } else if (const char* configured = std::getenv("NUAH_ATL_FRAMEWORK_RES");
      configured && *configured &&
      std::filesystem::is_regular_file(configured)) {
    framework = configured;
  }
  if (!std::filesystem::is_regular_file(framework)) {
    const auto build_framework = std::filesystem::path(root) /
                                 "res/framework-res/framework-res.apk";
    if (std::filesystem::is_regular_file(build_framework))
      framework = build_framework.string();
  }
  std::string natives = root + "/natives";
  // A local Meson build keeps its provider beside api-impl.jar instead of
  // installing a separate natives/ directory. Accept that layout directly so
  // local MVP runs do not need a copied runtime bundle.
  if (!std::filesystem::is_regular_file(
          std::filesystem::path(natives) / "libtranslation_layer_main.so") &&
      std::filesystem::is_regular_file(
          std::filesystem::path(root) / "libtranslation_layer_main.so")) {
    natives = root;
  }
  const auto apk_paths = apk_class_paths();
  if (apk_paths.empty()) {
    std::fprintf(stderr, "nuah ART: NUAH_APK_PATHS is empty\n");
    delete jvm;
    return nullptr;
  }
  // Roblox's Java classes live in base.apk.  Include a split only when it
  // actually contains classes; native/resource splits otherwise trigger ART's
  // duplicate-class and empty-dex slow paths.
  std::vector<std::string> app_classes;
  app_classes.push_back(apk_paths.front());
  jvm->class_path = api_jar + ":" + framework + ":" + join(app_classes);
  const std::string app_path = join(app_classes);
  static std::string app_apk_path;
  app_apk_path = app_classes.front();
  apk_path = app_apk_path.data();
  // App JNI libraries are unpacked into the private tree System.loadLibrary
  // uses. Keep the ART provider directory after it so framework natives
  // remain visible as well.
  std::string app_library_path = natives;
  if (const char* app_data = std::getenv("ANDROID_APP_DATA_DIR");
      app_data && *app_data) {
    const std::filesystem::path app_lib =
        std::filesystem::path(app_data) / "lib";
    const bool explicit_provider =
        (std::getenv("NUAH_ART_HOME") && *std::getenv("NUAH_ART_HOME")) ||
        (std::getenv("NUAH_ATL_HOME") && *std::getenv("NUAH_ATL_HOME")) ||
        (std::getenv("NUAH_ATL_NATIVE_DIR") &&
         *std::getenv("NUAH_ATL_NATIVE_DIR"));
    app_library_path = explicit_provider
        ? natives + ":" + app_lib.string()
        : app_lib.string() + ":" + natives;
  }
  const std::string library_path = app_library_path;
  std::string class_option = "-Djava.class.path=" + jvm->class_path;
  std::string app_class_option = "-Datl.app.class.path=" + app_path;
  std::string library_option = "-Djava.library.path=" + library_path;
  std::string app_library_option = "-Datl.app.library.path=" + library_path;
  /* The Android provider jars run inside host ART, so their default
   * /system/etc/security/cacerts does not exist.  Without a real root store
   * WolfSSL rejects the Roblox CDN certificate and its pre-warm task retries
   * while the scene is loading.  Reuse the distro-maintained Java store; a
   * caller may point at another JKS for diagnostics. */
  std::string trust_store_option;
  std::string trust_store_type_option;
  if (const char* configured = std::getenv("NUAH_JAVA_TRUST_STORE");
      configured && *configured &&
      std::filesystem::is_regular_file(configured)) {
    trust_store_option = std::string("-Djavax.net.ssl.trustStore=") +
                         configured;
    trust_store_type_option = "-Djavax.net.ssl.trustStoreType=JKS";
  } else if (!std::getenv("NUAH_DISABLE_JAVA_TRUST_STORE")) {
    static constexpr const char* kTrustStores[] = {
        "/etc/pki/java/cacerts",          // Fedora/RHEL
        "/etc/ssl/certs/java/cacerts",    // Debian/Ubuntu
        "/etc/ssl/certs/java/cacerts.jks" // Alpine derivatives
    };
    for (const char* candidate : kTrustStores) {
      if (!std::filesystem::is_regular_file(candidate)) continue;
      trust_store_option = std::string("-Djavax.net.ssl.trustStore=") +
                           candidate;
      trust_store_type_option = "-Djavax.net.ssl.trustStoreType=JKS";
      break;
    }
  }
  /* Roblox's Android HTTP stack opens several short-lived connections during
   * room startup.  On a desktop host with an incomplete IPv6 route, the
   * resolver/socket fallback can spend the full connect timeout on AAAA
   * addresses before trying IPv4.  That pause stops AssetProvider callbacks
   * and looks like a renderer stall even though Vulkan is idle.  Android's
   * networking properties are VM options, so set them before ART creates any
   * URL handlers.  Keep an opt-out for hosts with a working IPv6 path. */
  std::string prefer_ipv4_option =
      "-Djava.net.preferIPv4Stack=true";
  const bool prefer_ipv4 = [] {
    const char* value = std::getenv("NUAH_PREFER_IPV4");
    return !value || std::strcmp(value, "0") != 0;
  }();
  const std::string sdk_option = "-DBuild.VERSION.SDK_INT=36";
  std::string boot_append;
  const std::string dex_root = "/usr/local/lib64/java/dex/art";
  for (const char* jar : {"wolfssljni-hostdex.jar", "bouncycastle-hostdex.jar"}) {
    const std::string path = dex_root + "/" + jar;
    if (std::filesystem::is_regular_file(path)) {
      if (!boot_append.empty()) boot_append.push_back(':');
      boot_append += path;
    }
  }
  std::string boot_option;
  if (!boot_append.empty()) boot_option = "-Xbootclasspath/a:" + boot_append;

  const std::string bootclasspath = android16_bootclasspath();
  const std::filesystem::path boot_image =
      "/usr/local/lib64/java/dex/art/oat/boot.art";
  const bool use_boot_image = [] {
    const char* value = std::getenv("NUAH_ART_USE_BOOT_IMAGE");
    return !value || std::strcmp(value, "0") != 0;
  }();
  const bool have_boot_image =
      use_boot_image && !bootclasspath.empty() &&
      std::filesystem::is_regular_file(boot_image) &&
      std::filesystem::is_regular_file(boot_image.parent_path() / "boot.oat") &&
      std::filesystem::is_regular_file(boot_image.parent_path() / "boot.vdex");
  std::string bootclasspath_option;
  std::string image_option;
  if (have_boot_image) {
    bootclasspath_option = "-Xbootclasspath:" + bootclasspath;
    image_option = "-Ximage:" + boot_image.string();
    (void)::setenv("BOOTCLASSPATH", bootclasspath.c_str(), 1);
    if (trace_enabled()) {
      std::fprintf(stderr, "nuah ART: using boot image %s\n",
                   boot_image.c_str());
    }
  } else if (trace_enabled()) {
    std::fprintf(stderr,
                 "nuah ART: boot image unavailable; keeping imageless mode\n");
  }

  std::vector<JavaVMOption> options;
  for (std::string* option : {&library_option, &class_option, &app_class_option,
                              &app_library_option})
    options.push_back({option->data(), nullptr});
  if (!trust_store_option.empty()) {
    options.push_back({trust_store_option.data(), nullptr});
    options.push_back({trust_store_type_option.data(), nullptr});
  }
  if (prefer_ipv4) {
    options.push_back({prefer_ipv4_option.data(), nullptr});
  }
  /* ART's JNI checker is valuable while validating a new façade, but it
   * wraps every transition with argument/critical-section checks.  That is
   * measurable on Roblox's FunctionMarshal-heavy render path and can turn a
   * missed refresh into a visible hitch.  Keep it available for diagnostics,
   * but do not pay that cost during a normal playable launch. */
  const char* jni_check = std::getenv("NUAH_JNI_CHECK");
  if (jni_check && *jni_check && std::strcmp(jni_check, "0") != 0)
    options.push_back({"-Xcheck:jni", nullptr});
  options.push_back({const_cast<char*>(sdk_option.c_str()), nullptr});
  if (!bootclasspath_option.empty())
    options.push_back({bootclasspath_option.data(), nullptr});
  if (!image_option.empty()) {
    options.push_back({image_option.data(), nullptr});
  } else {
    options.push_back({"-Xnoimage-dex2oat", nullptr});
  }
  /* Keep the stable interpreter path for the current ART/Roblox pairing.
   * `-Xusejit:true` currently causes the Android image to unload through a
   * stale stdio callback during activity startup; it remains an explicit
   * diagnostic experiment rather than the playable default. */
  options.push_back({"-Xusejit:false", nullptr});
  if (!boot_option.empty()) options.push_back({boot_option.data(), nullptr});
  JavaVMInitArgs args{};
  args.version = JNI_VERSION_1_6;
  args.nOptions = static_cast<jint>(options.size());
  args.options = options.data();
  args.ignoreUnrecognized = JNI_FALSE;
  const jint result = create(&jvm->vm, reinterpret_cast<void**>(&jvm->env), &args);
  if (result != JNI_OK || !jvm->vm || !jvm->env) {
    std::fprintf(stderr, "nuah ART: JNI_CreateJavaVM failed (%d)\n", result);
    delete jvm;
    return nullptr;
  }
  jvm->vm_owner = ::pthread_self();
  jvm->vm_owner_attached = true;

  // ART resolves System.loadLibrary through the process bionic linker.
  // Open the same app-private file through that linker first; opening the
  // build-tree copy with glibc creates a second image.  The glibc path
  // remains a diagnostic fallback for hosts that do not expose bionic_dlopen.
  std::filesystem::path api_native_file =
      std::filesystem::path(natives) / "libtranslation_layer_main.so";
  if (!std::filesystem::is_regular_file(api_native_file)) {
    if (const char* app_data = std::getenv("ANDROID_APP_DATA_DIR");
        app_data && *app_data) {
      const auto app_provider = std::filesystem::path(app_data) / "lib" /
                                "libtranslation_layer_main.so";
      if (std::filesystem::is_regular_file(app_provider))
        api_native_file = app_provider;
    }
  }
  const std::string api_native_path = api_native_file.string();
  const auto bionic_dlopen = reinterpret_cast<NativeDlopen>(
      ::dlsym(RTLD_DEFAULT, "bionic_dlopen"));
  jvm->api_dlsym = reinterpret_cast<NativeDlsym>(
      ::dlsym(RTLD_DEFAULT, "bionic_dlsym"));

  // This is ATL's ownership boundary: Runtime.loadLibrary associates the
  // provider with the Android class loader, enabling ART's normal
  // Java_<class>_<method> resolver.  Calling glibc dlopen first bypasses that
  // association and forces a growing hand-written fallback table.
  const bool classloader_loaded = load_atl_library_for_classloader(jvm->env);
  if (classloader_loaded && bionic_dlopen && jvm->api_dlsym)
    jvm->api_native = bionic_dlopen(api_native_path.c_str(),
                                    RTLD_LAZY | RTLD_GLOBAL);
  // Some ATL builds do not expose the private Runtime overload.  In that
  // case still use the bionic linker (never glibc) before considering the
  // diagnostic fallback; this preserves one native-loader namespace.
  if (!jvm->api_native && bionic_dlopen && jvm->api_dlsym)
    jvm->api_native = bionic_dlopen(api_native_path.c_str(),
                                    RTLD_LAZY | RTLD_GLOBAL);
  if (!jvm->api_native) {
    jvm->api_dlsym = nullptr;
    jvm->api_native = dlopen(api_native_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
  }
  if (!jvm->api_native && trace_enabled())
    std::fprintf(stderr, "nuah ART: optional API native load failed: %s\n",
                 dlerror());
  if (jvm->api_native && trace_enabled())
    std::fprintf(stderr, "nuah ART: ATL provider loaded once from %s (%s linker, classloader=%s)\n",
                 api_native_path.c_str(), jvm->api_dlsym ? "bionic" : "glibc",
                 classloader_loaded ? "yes" : "no");
  if (!classloader_loaded && trace_enabled())
    std::fprintf(stderr,
                 "nuah ART: ATL class-loader registration unavailable; using provider handle only\n");

  // Only two Android-framework natives are touched while Roblox's classes
  // initialize.  Register these before resolving/allocating GameActivity;
  // its superclass initialization invokes Context's native configuration
  // hook.
  (void)register_host_native(
      jvm->env, "android/content/Context", "native_updateConfig",
      "(Landroid/content/res/Configuration;)V",
      reinterpret_cast<void*>(&nuah_art_context_update_config));
  (void)register_host_native(
      jvm->env, "android/util/Log", "println_native",
      "(IILjava/lang/String;Ljava/lang/String;)I",
      reinterpret_cast<void*>(&nuah_art_log_println));
  (void)register_host_native(
      jvm->env, "com/roblox/universalapp/logging/JNILoggingProtocol",
      "nativeLogEvent", "(Ljava/lang/String;J[Ljava/lang/Object;)V",
      reinterpret_cast<void*>(&nuah_art_logging_protocol_native_log_event));
  if (jvm->api_native) {
    // These entries are the provider's generated JNI contract, not hand
    // written Android replacements.  Registering the complete resource
    // parser surface once avoids the old fail/patch/rebuild loop while still
    // keeping the boundary limited to the classes ATL uses to parse an APK.
    static constexpr AtlNativeSpec kAssetAndXmlNatives[] = {
        {"android/content/res/AssetManager", "getPooledString",
         "(II)Ljava/lang/CharSequence;",
         "Java_android_content_res_AssetManager_getPooledString"},
        {"android/content/res/AssetManager", "list",
         "(Ljava/lang/String;)[Ljava/lang/String;",
         "Java_android_content_res_AssetManager_list"},
        {"android/content/res/AssetManager", "addAssetPathNative",
         "(Ljava/lang/String;)I",
         "Java_android_content_res_AssetManager_addAssetPathNative"},
        {"android/content/res/AssetManager", "isUpToDate", "()Z",
         "Java_android_content_res_AssetManager_isUpToDate"},
        {"android/content/res/AssetManager", "setLocale",
         "(Ljava/lang/String;)V",
         "Java_android_content_res_AssetManager_setLocale"},
        {"android/content/res/AssetManager", "getLocales",
         "()[Ljava/lang/String;",
         "Java_android_content_res_AssetManager_getLocales"},
        {"android/content/res/AssetManager", "setConfiguration",
         "(IILjava/lang/String;IIIIIIIIIIIIII)V",
         "Java_android_content_res_AssetManager_setConfiguration"},
        {"android/content/res/AssetManager", "getResourceIdentifier",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",
         "Java_android_content_res_AssetManager_getResourceIdentifier"},
        {"android/content/res/AssetManager", "getResourceName",
         "(I)Ljava/lang/String;",
         "Java_android_content_res_AssetManager_getResourceName"},
        {"android/content/res/AssetManager", "getResourcePackageName",
         "(I)Ljava/lang/String;",
         "Java_android_content_res_AssetManager_getResourcePackageName"},
        {"android/content/res/AssetManager", "getResourceTypeName",
         "(I)Ljava/lang/String;",
         "Java_android_content_res_AssetManager_getResourceTypeName"},
        {"android/content/res/AssetManager", "getResourceEntryName",
         "(I)Ljava/lang/String;",
         "Java_android_content_res_AssetManager_getResourceEntryName"},
        {"android/content/res/AssetManager", "openAsset",
         "(Ljava/lang/String;I)J",
         "Java_android_content_res_AssetManager_openAsset"},
        {"android/content/res/AssetManager", "openAssetFd",
         "(Ljava/lang/String;I[J[J)I",
         "Java_android_content_res_AssetManager_openAssetFd"},
        {"android/content/res/AssetManager", "destroyAsset", "(J)V",
         "Java_android_content_res_AssetManager_destroyAsset"},
        {"android/content/res/AssetManager", "readAssetChar", "(J)I",
         "Java_android_content_res_AssetManager_readAssetChar"},
        {"android/content/res/AssetManager", "readAsset", "(J[BJJ)I",
         "Java_android_content_res_AssetManager_readAsset"},
        {"android/content/res/AssetManager", "seekAsset", "(JJI)J",
         "Java_android_content_res_AssetManager_seekAsset"},
        {"android/content/res/AssetManager", "getAssetLength", "(J)J",
         "Java_android_content_res_AssetManager_getAssetLength"},
        {"android/content/res/AssetManager", "getAssetRemainingLength",
         "(J)J",
         "Java_android_content_res_AssetManager_getAssetRemainingLength"},
        {"android/content/res/AssetManager", "loadResourceValue",
         "(ISLandroid/util/TypedValue;Z)I",
         "Java_android_content_res_AssetManager_loadResourceValue"},
        {"android/content/res/AssetManager", "loadResourceBagValue",
         "(IILandroid/util/TypedValue;Z)I",
         "Java_android_content_res_AssetManager_loadResourceBagValue"},
        {"android/content/res/AssetManager", "applyStyle",
         "(JJII[IIJJ)V",
         "Java_android_content_res_AssetManager_applyStyle"},
        {"android/content/res/AssetManager", "resolveAttrs",
         "(JII[I[I[I[I)Z",
         "Java_android_content_res_AssetManager_resolveAttrs"},
        {"android/content/res/AssetManager", "retrieveAttributes",
         "(J[IIJJ)Z",
         "Java_android_content_res_AssetManager_retrieveAttributes"},
        {"android/content/res/AssetManager", "getArraySize", "(I)I",
         "Java_android_content_res_AssetManager_getArraySize"},
        {"android/content/res/AssetManager", "retrieveArray", "(I[I)I",
         "Java_android_content_res_AssetManager_retrieveArray"},
        {"android/content/res/AssetManager", "getStringBlockCount", "()I",
         "Java_android_content_res_AssetManager_getStringBlockCount"},
        {"android/content/res/AssetManager", "getNativeStringBlock", "(I)I",
         "Java_android_content_res_AssetManager_getNativeStringBlock"},
        {"android/content/res/AssetManager", "getCookieName",
         "(I)Ljava/lang/String;",
         "Java_android_content_res_AssetManager_getCookieName"},
        {"android/content/res/AssetManager", "getGlobalAssetCount", "()I",
         "Java_android_content_res_AssetManager_getGlobalAssetCount"},
        {"android/content/res/AssetManager", "getAssetAllocations",
         "()Ljava/lang/String;",
         "Java_android_content_res_AssetManager_getAssetAllocations"},
        {"android/content/res/AssetManager", "getGlobalAssetManagerCount",
         "()I",
         "Java_android_content_res_AssetManager_getGlobalAssetManagerCount"},
        {"android/content/res/AssetManager", "newTheme", "()J",
         "Java_android_content_res_AssetManager_newTheme"},
        {"android/content/res/AssetManager", "deleteTheme", "(J)V",
         "Java_android_content_res_AssetManager_deleteTheme"},
        {"android/content/res/AssetManager", "applyThemeStyle", "(JIZ)V",
         "Java_android_content_res_AssetManager_applyThemeStyle"},
        {"android/content/res/AssetManager", "copyTheme", "(JJ)V",
         "Java_android_content_res_AssetManager_copyTheme"},
        {"android/content/res/AssetManager", "loadThemeAttributeValue",
         "(JILandroid/util/TypedValue;Z)I",
         "Java_android_content_res_AssetManager_loadThemeAttributeValue"},
        {"android/content/res/AssetManager", "dumpTheme",
         "(JILjava/lang/String;Ljava/lang/String;)V",
         "Java_android_content_res_AssetManager_dumpTheme"},
        {"android/content/res/AssetManager", "openXmlAssetNative",
         "(ILjava/lang/String;)J",
         "Java_android_content_res_AssetManager_openXmlAssetNative"},
        {"android/content/res/AssetManager", "getArrayStringResource",
         "(I)[Ljava/lang/String;",
         "Java_android_content_res_AssetManager_getArrayStringResource"},
        {"android/content/res/AssetManager", "getArrayStringInfo", "(I)[I",
         "Java_android_content_res_AssetManager_getArrayStringInfo"},
        {"android/content/res/AssetManager", "init", "(I)V",
         "Java_android_content_res_AssetManager_init"},
        {"android/content/res/AssetManager", "native_setApkAssets",
         "([Ljava/lang/Object;I)V",
         "Java_android_content_res_AssetManager_native_1setApkAssets"},
        {"android/content/res/XmlBlock", "nativeCreate", "([BII)J",
         "Java_android_content_res_XmlBlock_nativeCreate"},
        {"android/content/res/XmlBlock", "nativeGetStringBlock", "(J)I",
         "Java_android_content_res_XmlBlock_nativeGetStringBlock"},
        {"android/content/res/XmlBlock", "nativeCreateParseState", "(J)J",
         "Java_android_content_res_XmlBlock_nativeCreateParseState"},
        {"android/content/res/XmlBlock", "nativeNext", "(J)I",
         "Java_android_content_res_XmlBlock_nativeNext"},
        {"android/content/res/XmlBlock", "nativeGetNamespace", "(J)I",
         "Java_android_content_res_XmlBlock_nativeGetNamespace"},
        {"android/content/res/XmlBlock", "nativeGetName",
         "(J)Ljava/lang/String;",
         "Java_android_content_res_XmlBlock_nativeGetName"},
        {"android/content/res/XmlBlock", "nativeGetText", "(J)I",
         "Java_android_content_res_XmlBlock_nativeGetText"},
        {"android/content/res/XmlBlock", "nativeGetLineNumber", "(J)I",
         "Java_android_content_res_XmlBlock_nativeGetLineNumber"},
        {"android/content/res/XmlBlock", "nativeGetAttributeCount", "(J)I",
         "Java_android_content_res_XmlBlock_nativeGetAttributeCount"},
        {"android/content/res/XmlBlock", "nativeGetAttributeNamespace",
         "(JI)I",
         "Java_android_content_res_XmlBlock_nativeGetAttributeNamespace"},
        {"android/content/res/XmlBlock", "nativeGetAttributeName", "(JI)I",
         "Java_android_content_res_XmlBlock_nativeGetAttributeName"},
        {"android/content/res/XmlBlock", "nativeGetAttributeResource",
         "(JI)I",
         "Java_android_content_res_XmlBlock_nativeGetAttributeResource"},
        {"android/content/res/XmlBlock", "nativeGetAttributeDataType",
         "(JI)I",
         "Java_android_content_res_XmlBlock_nativeGetAttributeDataType"},
        {"android/content/res/XmlBlock", "nativeGetAttributeData", "(JI)I",
         "Java_android_content_res_XmlBlock_nativeGetAttributeData"},
        {"android/content/res/XmlBlock", "nativeGetAttributeStringValue",
         "(JI)Ljava/lang/String;",
         "Java_android_content_res_XmlBlock_nativeGetAttributeStringValue"},
        {"android/content/res/XmlBlock", "nativeGetIdAttribute", "(J)I",
         "Java_android_content_res_XmlBlock_nativeGetIdAttribute"},
        {"android/content/res/XmlBlock", "nativeGetClassAttribute",
         "(J)Ljava/lang/String;",
         "Java_android_content_res_XmlBlock_nativeGetClassAttribute"},
        {"android/content/res/XmlBlock", "nativeGetStyleAttribute", "(J)I",
         "Java_android_content_res_XmlBlock_nativeGetStyleAttribute"},
        {"android/content/res/XmlBlock", "nativeGetAttributeIndex",
         "(JLjava/lang/String;Ljava/lang/String;)I",
         "Java_android_content_res_XmlBlock_nativeGetAttributeIndex"},
        {"android/content/res/XmlBlock", "nativeDestroyParseState", "(J)V",
         "Java_android_content_res_XmlBlock_nativeDestroyParseState"},
        {"android/content/res/XmlBlock", "nativeGetPooledString", "(JI)Ljava/lang/String;",
         "Java_android_content_res_XmlBlock_nativeGetPooledString"},
        {"android/content/res/XmlBlock", "nativeDestroy", "(J)V",
         "Java_android_content_res_XmlBlock_nativeDestroy"},
        // Environment is initialized while ATL's Context/AssetManager classes
        // are loaded. Use the provider's own implementation and app root.
        {"android/os/Environment", "native_get_app_data_dir",
         "()Ljava/lang/String;",
         "Java_android_os_Environment_native_1get_1app_1data_1dir"},
        {"android/content/Context", "native_get_apk_path",
         "()Ljava/lang/String;",
         "Java_android_content_Context_native_1get_1apk_1path"},
        {"android/os/SystemClock", "uptimeMillis", "()J",
         "Java_android_os_SystemClock_uptimeMillis"},
        {"android/os/SystemClock", "elapsedRealtime", "()J",
         "Java_android_os_SystemClock_elapsedRealtime"},
        {"android/os/SystemClock", "elapsedRealtimeNanos", "()J",
         "Java_android_os_SystemClock_elapsedRealtimeNanos"},
        {"android/os/SystemClock", "currentThreadTimeMillis", "()J",
         "Java_android_os_SystemClock_currentThreadTimeMillis"},
        {"android/os/MessageQueue", "nativeInit", "()J",
         "Java_android_os_MessageQueue_nativeInit"},
        {"android/os/MessageQueue", "nativeDestroy", "(J)V",
         "Java_android_os_MessageQueue_nativeDestroy"},
        {"android/os/MessageQueue", "nativePollOnce", "(JI)Z",
         "Java_android_os_MessageQueue_nativePollOnce"},
        {"android/os/MessageQueue", "nativeWake", "(J)V",
         "Java_android_os_MessageQueue_nativeWake"},
        {"android/os/MessageQueue", "nativeIsIdling", "(J)Z",
         "Java_android_os_MessageQueue_nativeIsIdling"},
        // ATL ships the complete SQLite JNI implementation. Register its
        // exported methods with ART; otherwise MainGameActivity's normal
        // AppManager bootstrap dies in a background billing/cookie task at
        // SQLiteConnection.nativeOpen before Roblox can initialize.
        {"android/database/sqlite/SQLiteGlobal", "nativeReleaseMemory", "()I",
         "Java_android_database_sqlite_SQLiteGlobal_nativeReleaseMemory"},
        {"android/database/sqlite/SQLiteConnection", "nativeOpen",
         "(Ljava/lang/String;ILjava/lang/String;ZZ)J",
         "Java_android_database_sqlite_SQLiteConnection_nativeOpen"},
        {"android/database/sqlite/SQLiteConnection", "nativeClose", "(J)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeClose"},
        {"android/database/sqlite/SQLiteConnection", "nativeRegisterCustomFunction",
         "(JLandroid/database/sqlite/SQLiteCustomFunction;)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeRegisterCustomFunction"},
        {"android/database/sqlite/SQLiteConnection", "nativeRegisterLocalizedCollators",
         "(JLjava/lang/String;)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeRegisterLocalizedCollators"},
        {"android/database/sqlite/SQLiteConnection", "nativePrepareStatement",
         "(JLjava/lang/String;)J",
         "Java_android_database_sqlite_SQLiteConnection_nativePrepareStatement"},
        {"android/database/sqlite/SQLiteConnection", "nativeFinalizeStatement",
         "(JJ)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeFinalizeStatement"},
        {"android/database/sqlite/SQLiteConnection", "nativeGetParameterCount",
         "(JJ)I",
         "Java_android_database_sqlite_SQLiteConnection_nativeGetParameterCount"},
        {"android/database/sqlite/SQLiteConnection", "nativeIsReadOnly", "(JJ)Z",
         "Java_android_database_sqlite_SQLiteConnection_nativeIsReadOnly"},
        {"android/database/sqlite/SQLiteConnection", "nativeGetColumnCount", "(JJ)I",
         "Java_android_database_sqlite_SQLiteConnection_nativeGetColumnCount"},
        {"android/database/sqlite/SQLiteConnection", "nativeGetColumnName",
         "(JJI)Ljava/lang/String;",
         "Java_android_database_sqlite_SQLiteConnection_nativeGetColumnName"},
        {"android/database/sqlite/SQLiteConnection", "nativeBindNull", "(JJI)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeBindNull"},
        {"android/database/sqlite/SQLiteConnection", "nativeBindLong", "(JJIJ)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeBindLong"},
        {"android/database/sqlite/SQLiteConnection", "nativeBindDouble", "(JJID)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeBindDouble"},
        {"android/database/sqlite/SQLiteConnection", "nativeBindString",
         "(JJILjava/lang/String;)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeBindString"},
        {"android/database/sqlite/SQLiteConnection", "nativeBindBlob", "(JJI[B)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeBindBlob"},
        {"android/database/sqlite/SQLiteConnection",
         "nativeResetStatementAndClearBindings", "(JJ)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeResetStatementAndClearBindings"},
        {"android/database/sqlite/SQLiteConnection", "nativeExecute", "(JJ)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeExecute"},
        {"android/database/sqlite/SQLiteConnection", "nativeExecuteForLong", "(JJ)J",
         "Java_android_database_sqlite_SQLiteConnection_nativeExecuteForLong"},
        {"android/database/sqlite/SQLiteConnection", "nativeExecuteForString",
         "(JJ)Ljava/lang/String;",
         "Java_android_database_sqlite_SQLiteConnection_nativeExecuteForString"},
        {"android/database/sqlite/SQLiteConnection",
         "nativeExecuteForChangedRowCount", "(JJ)I",
         "Java_android_database_sqlite_SQLiteConnection_nativeExecuteForChangedRowCount"},
        {"android/database/sqlite/SQLiteConnection",
         "nativeExecuteForLastInsertedRowId", "(JJ)J",
         "Java_android_database_sqlite_SQLiteConnection_nativeExecuteForLastInsertedRowId"},
        {"android/database/sqlite/SQLiteConnection", "nativeExecuteForCursorWindow",
         "(JJLandroid/database/CursorWindow;IIZ)J",
         "Java_android_database_sqlite_SQLiteConnection_nativeExecuteForCursorWindow"},
        {"android/database/sqlite/SQLiteConnection", "nativeGetDbLookaside", "(J)I",
         "Java_android_database_sqlite_SQLiteConnection_nativeGetDbLookaside"},
        {"android/database/sqlite/SQLiteConnection", "nativeCancel", "(J)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeCancel"},
        {"android/database/sqlite/SQLiteConnection", "nativeResetCancel", "(JZ)V",
         "Java_android_database_sqlite_SQLiteConnection_nativeResetCancel"},
        {"android/database/sqlite/SQLiteConnection", "nativeHasCodec", "()Z",
         "Java_android_database_sqlite_SQLiteConnection_nativeHasCodec"},
        {"android/net/ConnectivityManager", "registerNetworkCallback",
         "(Landroid/net/NetworkRequest;Landroid/net/ConnectivityManager$NetworkCallback;)V",
         "Java_android_net_ConnectivityManager_registerNetworkCallback"},
        {"android/net/ConnectivityManager", "isActiveNetworkMetered", "()Z",
         "Java_android_net_ConnectivityManager_isActiveNetworkMetered"},
        {"android/net/ConnectivityManager", "nativeGetNetworkAvailable", "()Z",
         "Java_android_net_ConnectivityManager_nativeGetNetworkAvailable"},
        // GameActivity creates this real ATL SurfaceView during its Android
        // lifecycle. Keep the provider's widget/surface implementation on
        // the JNI side; Nuah only supplies the host Vulkan window later.
        {"android/view/SurfaceView", "native_constructor",
         "(Landroid/content/Context;Landroid/util/AttributeSet;)J",
         "Java_android_view_SurfaceView_native_1constructor"},
        {"android/view/SurfaceView", "nativeDirectAppStart", "()V",
         "Java_android_view_SurfaceView_nativeDirectAppStart"},
        {"android/view/SurfaceView", "native_createSnapshot", "()J",
         "Java_android_view_SurfaceView_native_1createSnapshot"},
        {"android/view/SurfaceView", "native_postSnapshot", "(JJ)V",
         "Java_android_view_SurfaceView_native_1postSnapshot"},
        {"android/view/inputmethod/InputMethodManager", "nativeInit", "()J",
         "Java_android_view_inputmethod_InputMethodManager_nativeInit"},
        {"android/view/inputmethod/InputMethodManager", "nativeShowSoftInput",
         "(JJLandroid/view/inputmethod/InputConnection;I)Z",
         "Java_android_view_inputmethod_InputMethodManager_nativeShowSoftInput"},
        {"android/view/inputmethod/InputMethodManager", "nativeHideSoftInput",
         "(J)V",
         "Java_android_view_inputmethod_InputMethodManager_nativeHideSoftInput"},
        {"android/view/ViewGroup", "native_addView",
         "(JJILandroid/view/ViewGroup$LayoutParams;)V",
         "Java_android_view_ViewGroup_native_1addView"},
        {"android/view/ViewGroup", "native_removeView", "(JJ)V",
         "Java_android_view_ViewGroup_native_1removeView"},
        {"android/view/ViewGroup", "native_drawChildren", "(JJ)V",
         "Java_android_view_ViewGroup_native_1drawChildren"},
        {"android/view/ViewGroup", "native_drawChild", "(JJJ)V",
         "Java_android_view_ViewGroup_native_1drawChild"},
        {"android/view/ViewGroup", "native_dispatchTouchEvent",
         "(JLandroid/view/MotionEvent;DD)Z",
         "Java_android_view_ViewGroup_native_1dispatchTouchEvent"},
        {"android/graphics/drawable/Drawable", "native_paintable_from_path",
         "(Ljava/lang/String;)J",
         "Java_android_graphics_drawable_Drawable_native_1paintable_1from_1path"},
        {"android/graphics/drawable/Drawable", "native_constructor", "()J",
         "Java_android_graphics_drawable_Drawable_native_1constructor"},
        {"android/graphics/drawable/Drawable", "native_invalidate", "(J)V",
         "Java_android_graphics_drawable_Drawable_native_1invalidate"},
        {"android/graphics/drawable/Drawable", "native_draw", "(JJII)V",
         "Java_android_graphics_drawable_Drawable_native_1draw"},
        {"android/graphics/drawable/Drawable", "native_ref", "(J)V",
         "Java_android_graphics_drawable_Drawable_native_1ref"},
        {"android/graphics/drawable/Drawable", "native_unref", "(J)V",
         "Java_android_graphics_drawable_Drawable_native_1unref"},
        // Matrix is part of the framework API jar, but ART resolves its
        // private natives lazily.  ATL already ships the generated JNI
        // implementation; expose that implementation to this class loader
        // before Activity/View code asks Matrix to allocate its handle.
        {"android/graphics/Matrix", "native_create", "(J)J",
         "Java_android_graphics_Matrix_native_1create"},
        {"android/graphics/Matrix", "native_getValues", "(J[F)V",
         "Java_android_graphics_Matrix_native_1getValues"},
        {"android/graphics/Matrix", "native_set", "(JJ)V",
         "Java_android_graphics_Matrix_native_1set"},
        {"android/graphics/Matrix", "native_isIdentity", "(J)Z",
         "Java_android_graphics_Matrix_native_1isIdentity"},
        {"android/graphics/Matrix", "native_rectStaysRect", "(J)Z",
         "Java_android_graphics_Matrix_native_1rectStaysRect"},
        {"android/graphics/Matrix", "native_reset", "(J)V",
         "Java_android_graphics_Matrix_native_1reset"},
        {"android/graphics/Matrix", "native_setTranslate", "(JFF)V",
         "Java_android_graphics_Matrix_native_1setTranslate"},
        {"android/graphics/Matrix", "native_setScale", "(JFF)V",
         "Java_android_graphics_Matrix_native_1setScale__JFF"},
        {"android/graphics/Matrix", "native_setScale", "(JFFFF)V",
         "Java_android_graphics_Matrix_native_1setScale__JFFFF"},
        {"android/graphics/Matrix", "native_setRotate", "(JF)V",
         "Java_android_graphics_Matrix_native_1setRotate__JF"},
        {"android/graphics/Matrix", "native_setRotate", "(JFFF)V",
         "Java_android_graphics_Matrix_native_1setRotate__JFFF"},
        {"android/graphics/Matrix", "native_preTranslate", "(JFF)Z",
         "Java_android_graphics_Matrix_native_1preTranslate"},
        {"android/graphics/Matrix", "native_preScale", "(JFF)Z",
         "Java_android_graphics_Matrix_native_1preScale__JFF"},
        {"android/graphics/Matrix", "native_preScale", "(JFFFF)Z",
         "Java_android_graphics_Matrix_native_1preScale__JFFFF"},
        {"android/graphics/Matrix", "native_preRotate", "(JF)Z",
         "Java_android_graphics_Matrix_native_1preRotate__JF"},
        {"android/graphics/Matrix", "native_preRotate", "(JFFF)Z",
         "Java_android_graphics_Matrix_native_1preRotate__JFFF"},
        {"android/graphics/Matrix", "native_preConcat", "(JJ)Z",
         "Java_android_graphics_Matrix_native_1preConcat"},
        {"android/graphics/Matrix", "native_postTranslate", "(JFF)Z",
         "Java_android_graphics_Matrix_native_1postTranslate"},
        {"android/graphics/Matrix", "native_postScale", "(JFF)Z",
         "Java_android_graphics_Matrix_native_1postScale__JFF"},
        {"android/graphics/Matrix", "native_postScale", "(JFFFF)Z",
         "Java_android_graphics_Matrix_native_1postScale__JFFFF"},
        {"android/graphics/Matrix", "native_postRotate", "(JF)Z",
         "Java_android_graphics_Matrix_native_1postRotate__JF"},
        {"android/graphics/Matrix", "native_postRotate", "(JFFF)Z",
         "Java_android_graphics_Matrix_native_1postRotate__JFFF"},
        {"android/graphics/Matrix", "native_postConcat", "(JJ)Z",
         "Java_android_graphics_Matrix_native_1postConcat"},
        {"android/graphics/Matrix", "native_setRectToRect",
         "(JLandroid/graphics/RectF;Landroid/graphics/RectF;I)Z",
         "Java_android_graphics_Matrix_native_1setRectToRect"},
        {"android/graphics/Matrix", "native_invert", "(JJ)Z",
         "Java_android_graphics_Matrix_native_1invert"},
        {"android/graphics/Matrix", "native_mapPoints",
         "(J[FI[FIIZ)V", "Java_android_graphics_Matrix_native_1mapPoints"},
        {"android/graphics/Matrix", "native_mapRect",
         "(JLandroid/graphics/RectF;Landroid/graphics/RectF;)Z",
         "Java_android_graphics_Matrix_native_1mapRect"},
        {"android/graphics/Matrix", "native_setValues", "(J[F)V",
         "Java_android_graphics_Matrix_native_1setValues"},
        {"android/graphics/Matrix", "native_equals", "(JJ)Z",
         "Java_android_graphics_Matrix_native_1equals"},
        {"android/graphics/Matrix", "finalizer", "(J)V",
         "Java_android_graphics_Matrix_finalizer"},
        // ATL provides the generated Paint implementation. Register the
        // complete small surface before framework startup constructs Paint;
        // otherwise ART aborts at Paint.native_create().
        {"android/graphics/Paint", "native_create", "()J",
         "Java_android_graphics_Paint_native_1create"},
        {"android/graphics/Paint", "native_clone", "(J)J",
         "Java_android_graphics_Paint_native_1clone"},
        {"android/graphics/Paint", "native_recycle", "(J)V",
         "Java_android_graphics_Paint_native_1recycle"},
        {"android/graphics/Paint", "native_set_color", "(JI)V",
         "Java_android_graphics_Paint_native_1set_1color"},
        {"android/graphics/Paint", "native_get_color", "(J)I",
         "Java_android_graphics_Paint_native_1get_1color"},
        {"android/graphics/Paint", "native_set_alpha", "(JI)V",
         "Java_android_graphics_Paint_native_1set_1alpha"},
        {"android/graphics/Paint", "native_get_alpha", "(J)I",
         "Java_android_graphics_Paint_native_1get_1alpha"},
        {"android/graphics/Paint", "native_set_style", "(JI)V",
         "Java_android_graphics_Paint_native_1set_1style"},
        {"android/graphics/Paint", "native_get_style", "(J)I",
         "Java_android_graphics_Paint_native_1get_1style"},
        {"android/graphics/Paint", "native_set_stroke_width", "(JF)V",
         "Java_android_graphics_Paint_native_1set_1stroke_1width"},
        {"android/graphics/Paint", "native_get_stroke_width", "(J)F",
         "Java_android_graphics_Paint_native_1get_1stroke_1width"},
        {"android/graphics/Paint", "native_set_stroke_cap", "(JI)V",
         "Java_android_graphics_Paint_native_1set_1stroke_1cap"},
        {"android/graphics/Paint", "native_get_stroke_cap", "(J)I",
         "Java_android_graphics_Paint_native_1get_1stroke_1cap"},
        {"android/graphics/Paint", "native_set_stroke_join", "(JI)V",
         "Java_android_graphics_Paint_native_1set_1stroke_1join"},
        {"android/graphics/Paint", "native_get_stroke_join", "(J)I",
         "Java_android_graphics_Paint_native_1get_1stroke_1join"},
        {"android/graphics/Paint", "native_set_text_size", "(JF)V",
         "Java_android_graphics_Paint_native_1set_1text_1size"},
        {"android/graphics/Paint", "native_get_text_size", "(J)F",
         "Java_android_graphics_Paint_native_1get_1text_1size"},
        {"android/graphics/Paint", "native_set_color_filter", "(JII)V",
         "Java_android_graphics_Paint_native_1set_1color_1filter"},
        {"android/graphics/Paint", "native_get_text_bounds",
         "(JLjava/lang/String;Landroid/graphics/Rect;)V",
         "Java_android_graphics_Paint_native_1get_1text_1bounds"},
        {"android/graphics/Paint", "native_set_text_align", "(JI)V",
         "Java_android_graphics_Paint_native_1set_1text_1align"},
        // ATL constructs the Activity's Window/FrameLayout during
        // attachBaseContext. Its View native constructor is the only widget
        // entry needed before Roblox's GameActivity callback registration.
        {"android/view/View", "native_constructor",
         "(Landroid/content/Context;Landroid/util/AttributeSet;)J",
         "Java_android_view_View_native_1constructor"},
        {"android/view/View", "nativeSetOnTouchListener", "(J)V",
         "Java_android_view_View_nativeSetOnTouchListener"},
        {"android/view/View", "nativeSetOnClickListener", "(J)V",
         "Java_android_view_View_nativeSetOnClickListener"},
        {"android/view/View", "native_setLayoutParams",
         "(JIIIFIIII)V",
         "Java_android_view_View_native_1setLayoutParams"},
        {"android/view/View", "native_destructor", "(J)V",
         "Java_android_view_View_native_1destructor"},
        {"android/view/View", "native_measure", "(JII)V",
         "Java_android_view_View_native_1measure"},
        {"android/view/View", "native_layout", "(JIIII)V",
         "Java_android_view_View_native_1layout"},
        {"android/view/View", "native_requestLayout", "(J)V",
         "Java_android_view_View_native_1requestLayout"},
        {"android/view/View", "native_setBackgroundDrawable", "(JJ)V",
         "Java_android_view_View_native_1setBackgroundDrawable"},
        {"android/view/View", "native_queueAllocate", "(J)V",
         "Java_android_view_View_native_1queueAllocate"},
        {"android/view/View", "native_addClass", "(JLjava/lang/String;)V",
         "Java_android_view_View_native_1addClass"},
        {"android/view/View", "native_removeClass", "(JLjava/lang/String;)V",
         "Java_android_view_View_native_1removeClass"},
        {"android/view/View", "native_addClasses", "(J[Ljava/lang/String;)V",
         "Java_android_view_View_native_1addClasses"},
        {"android/view/View", "native_removeClasses", "(J[Ljava/lang/String;)V",
         "Java_android_view_View_native_1removeClasses"},
        {"android/view/View", "native_drawBackground", "(JJ)V",
         "Java_android_view_View_native_1drawBackground"},
        {"android/view/View", "native_drawContent", "(JJ)V",
         "Java_android_view_View_native_1drawContent"},
        {"android/view/View", "nativeRequestFocus", "(JI)V",
         "Java_android_view_View_nativeRequestFocus"},
        {"android/view/View", "nativeSetFullscreen", "(JZ)V",
         "Java_android_view_View_nativeSetFullscreen"},
        {"android/view/View", "native_get_window", "(J)Landroid/view/Window;",
         "Java_android_view_View_native_1get_1window"},
        {"android/view/View", "nativeInvalidate", "(J)V",
         "Java_android_view_View_nativeInvalidate"},
        {"android/view/View", "native_setBackgroundColor", "(JI)V",
         "Java_android_view_View_native_1setBackgroundColor"},
        {"android/view/View", "native_setVisibility", "(JIFZ)V",
         "Java_android_view_View_native_1setVisibility"},
        {"android/view/View", "native_setPadding", "(JIIII)V",
         "Java_android_view_View_native_1setPadding"},
        {"android/view/View", "nativeSetOnLongClickListener", "(J)V",
         "Java_android_view_View_nativeSetOnLongClickListener"},
        {"android/view/View", "nativeIsFocused", "(J)Z",
         "Java_android_view_View_nativeIsFocused"},
        {"android/view/View", "native_getMatrix", "(JJ)Z",
         "Java_android_view_View_native_1getMatrix"},
        {"android/view/View", "nativeIsAttachedToWindow", "(J)Z",
         "Java_android_view_View_nativeIsAttachedToWindow"},
        {"android/view/View", "native_keep_screen_on", "(JZ)V",
         "Java_android_view_View_native_1keep_1screen_1on"},
        {"android/view/View", "native_getGlobalVisibleRect",
         "(JLandroid/graphics/Rect;)Z",
         "Java_android_view_View_native_1getGlobalVisibleRect"},
        {"android/view/View", "getWindowVisibleDisplayFrame",
         "(Landroid/graphics/Rect;)V",
         "Java_android_view_View_getWindowVisibleDisplayFrame"},
        {"android/view/Window", "set_jobject", "(JLandroid/view/Window;)V",
         "Java_android_view_Window_set_1jobject"},
        {"android/view/Window", "set_widget_as_root", "(JJ)V",
         "Java_android_view_Window_set_1widget_1as_1root"},
        {"android/view/Window", "set_title", "(JLjava/lang/String;)V",
         "Java_android_view_Window_set_1title"},
        {"android/view/Window", "take_input_queue",
         "(JLandroid/view/InputQueue$Callback;Landroid/view/InputQueue;)V",
         "Java_android_view_Window_take_1input_1queue"},
        {"android/view/Window", "set_layout", "(JII)V",
         "Java_android_view_Window_set_1layout"},
        {"android/view/Window", "remove_gtk_background", "(J)V",
         "Java_android_view_Window_remove_1gtk_1background"},
        {"android/view/Window", "set_screen_brightness", "(F)V",
         "Java_android_view_Window_set_1screen_1brightness"},
    };
    register_atl_natives(jvm->env, jvm->api_native, jvm->api_dlsym,
                         kAssetAndXmlNatives,
                         sizeof(kAssetAndXmlNatives) /
                             sizeof(kAssetAndXmlNatives[0]));
  }
  initialize_atl_host_state(jvm);
  // Registering ATL's framework natives above is required before touching
  // Build.VERSION: its static initializer logs through Log.println_native.
  force_android_sdk_level(jvm->env, 36);
  if (!initialize_roblox_device_static_params(jvm) && trace_enabled()) {
    std::fprintf(stderr,
                 "nuah ART: Roblox DeviceStaticParams setup was unavailable\n");
  }

  jclass activity_class = find_class(jvm->env, "com/google/androidgamesdk/GameActivity");
  if (!activity_class) {
    std::fprintf(stderr, "nuah ART: GameActivity is not on the APK class path\n");
    delete jvm;
    return nullptr;
  }
  // Activity/AppCompat construction expects a main Looper, which Android's
  // ActivityThread normally prepares before instantiating GameActivity. Nuah
  // has no ActivityThread, so establish the documented main looper once on
  // this ART thread before invoking the real constructor.
  jclass looper_class = find_class(jvm->env, "android/os/Looper");
  if (looper_class) {
    const jmethodID prepare =
        jvm->env->GetStaticMethodID(looper_class, "prepareMainLooper", "()V");
    if (prepare) jvm->env->CallStaticVoidMethod(looper_class, prepare);
    clear_exception(jvm->env, "Looper.prepareMainLooper");
  }
  // Do not allocate the activity directly.  ATL's Activity constructor is
  // intentionally `super(null)`; the normal Android/ATL launcher immediately
  // calls ATLLoadedApp.createActivity(), which attaches a ContextImpl and
  // creates the Window object.  Bypassing that path leaves ContextWrapper's
  // base null and Roblox later crashes in Context.getResources().
  // Activity.internalCreateActivity is ATL's small, public bridge for exactly
  // this native-launch case.  MainGameActivity is the real Roblox subclass of
  // GameActivity, so using it also preserves the app's Java-side state.
  jobject activity = nullptr;
  jclass activity_helper = find_class(jvm->env, "android/app/Activity");
  jclass intent_class = find_class(jvm->env, "android/content/Intent");
  const jmethodID intent_ctor =
      intent_class ? jvm->env->GetMethodID(intent_class, "<init>", "()V")
                   : nullptr;
  jobject intent = intent_ctor ? jvm->env->NewObject(intent_class, intent_ctor)
                               : nullptr;
  const jmethodID create_activity = activity_helper
      ? jvm->env->GetStaticMethodID(
            activity_helper, "internalCreateActivity",
            "(Ljava/lang/String;JLandroid/content/Intent;)Landroid/app/Activity;")
      : nullptr;
  jstring main_activity_name =
      jvm->env->NewStringUTF("com.roblox.client.startup.MainGameActivity");
  if (create_activity && intent && main_activity_name) {
    activity = jvm->env->CallStaticObjectMethod(
        activity_helper, create_activity, main_activity_name,
        static_cast<jlong>(0), intent);
  }
  if (!activity || jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "ATL Activity.internalCreateActivity");
    activity = nullptr;
  }
  if (!activity) {
    std::fprintf(stderr,
                 "nuah ART: ATL could not construct MainGameActivity with an attached Context\n");
    delete jvm;
    return nullptr;
  }
  jvm->activity = jvm->env->NewGlobalRef(activity);
  clear_exception(jvm->env, "GameActivity global reference");
  if (!jvm->activity) {
    delete jvm;
    return nullptr;
  }
  if (trace_enabled())
    std::fprintf(stderr, "nuah ART: VM ready (%s) activity=%p\n",
                 art_path().c_str(), static_cast<void*>(jvm->activity));
  return jvm;
}

extern "C" void nuah_jvm_destroy(NuahJvm* jvm) {
  if (!jvm) return;
  nuah_jvm_clear_surface(jvm);
  delete_global(jvm->env, jvm->activity);
  delete_global(jvm->env, jvm->key_event);
  delete_global(jvm->env, jvm->motion_event);
  delete_global_class(jvm->env, jvm->native_input_interface);
  delete_global_class(jvm->env, jvm->native_gl_interface);
  // ART cannot safely be destroyed while Roblox worker threads are alive.  It
  // is intentionally kept resident until process exit, just like ATL's VM.
  /* Detach only the thread that called JNI_CreateJavaVM.  The VM remains
   * resident; this merely closes ART's TLS/thread-state record before the
   * isolated pthread exits. */
  if (jvm->vm && jvm->vm_owner_attached &&
      ::pthread_equal(jvm->vm_owner, ::pthread_self())) {
    (void)jvm->vm->DetachCurrentThread();
    jvm->vm_owner_attached = false;
  }
  delete jvm;
}

extern "C" void* nuah_jvm_java_vm(NuahJvm* jvm) {
  return jvm ? static_cast<void*>(jvm->vm) : nullptr;
}

extern "C" void* nuah_jvm_jni_env(NuahJvm* jvm) {
  return jvm ? static_cast<void*>(jvm->env) : nullptr;
}

extern "C" void* nuah_jvm_find_registered_native(
    NuahJvm* jvm, const char* class_name, const char* method_name,
    const char* signature) {
  if (!jvm || !class_name || !method_name || !signature) return nullptr;
  jmethodID method = find_instance_method(jvm->env, class_name, method_name, signature);
  // The real ART registry is private.  The method ID is a stable, non-null
  // proof that JNI_OnLoad installed/resolved the contract; dispatch below
  // invokes it through ART rather than casting it to a C callback.
  return method ? reinterpret_cast<void*>(method) : nullptr;
}

extern "C" int nuah_jvm_bind_native(NuahJvm* jvm, const char* class_name,
                                      const char* method_name,
                                      const char* signature, void* function) {
  if (!jvm || !function) return 0;
  jclass klass = find_class(jvm->env, class_name);
  if (!klass) return 0;
  const JNINativeMethod method{method_name, signature, function};
  const jint result = jvm->env->RegisterNatives(klass, &method, 1);
  if (trace_enabled()) {
    std::fprintf(stderr,
                 "nuah ART: RegisterNatives %s.%s %s fn=%p result=%d\n",
                 class_name, method_name, signature, function,
                 static_cast<int>(result));
  }
  if (result != JNI_OK) clear_exception(jvm->env, "RegisterNatives");
  return result == JNI_OK;
}

extern "C" void* nuah_jvm_game_activity(NuahJvm* jvm) {
  return jvm ? jvm->activity : nullptr;
}

extern "C" int nuah_jvm_dispatch_application_create(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return 0;
  if (trace_enabled()) std::fprintf(stderr, "nuah ART: app-state Context.createApplication begin\n");
  jclass context_class = find_class(jvm->env, "android/content/Context");
  if (!context_class) return 0;
  const jmethodID create_application = jvm->env->GetStaticMethodID(
      context_class, "createApplication", "(J)Landroid/app/Application;");
  if (!create_application) {
    clear_exception(jvm->env, "Context.createApplication lookup");
    return 0;
  }
  jobject application = jvm->env->CallStaticObjectMethod(
      context_class, create_application, static_cast<jlong>(0));
  if (!application || jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "Context.createApplication");
    return 0;
  }
  if (trace_enabled()) std::fprintf(stderr, "nuah ART: app-state Context.createApplication done\n");

  // Roblox disables AndroidX Startup's WorkManager provider in its manifest,
  // but MainGameActivity still uses WorkManager during onCreate.  Invoke the
  // library's own Initializer contract once, with the real Application
  // object, instead of recreating WorkManager or adding framework shims.
  if (!std::getenv("NUAH_SKIP_WORKMANAGER_INIT")) {
    jclass work_initializer =
        find_class(jvm->env, "androidx/work/WorkManagerInitializer");
    if (work_initializer) {
      const jmethodID initializer_ctor =
          jvm->env->GetMethodID(work_initializer, "<init>", "()V");
      const jmethodID initializer_create = jvm->env->GetMethodID(
          work_initializer, "create",
          "(Landroid/content/Context;)Ljava/lang/Object;");
      if (initializer_ctor && initializer_create) {
        jobject initializer =
            jvm->env->NewObject(work_initializer, initializer_ctor);
        if (initializer) {
          (void)jvm->env->CallObjectMethod(initializer, initializer_create,
                                           application);
          if (jvm->env->ExceptionCheck())
            clear_exception(jvm->env, "WorkManagerInitializer.create");
        }
      } else {
        clear_exception(jvm->env, "WorkManagerInitializer methods");
      }
    }
  } else if (trace_enabled()) {
    std::fprintf(stderr, "nuah ART: skipping WorkManager pre-initialization\n");
  }

  // Do not call RobloxApplication.onCreate here.  That method is the full
  // Android production bootstrap (WorkManager, analytics, advertising,
  // network providers, etc.) and assumes a complete Android system. Calling
  // it before MainGameActivity exists turns every optional service into a
  // process-fatal missing-API probe. The game activity only needs the small
  // process state that onCreate normally establishes before it starts the
  // native client; install that state explicitly and let Activity.onCreate be
  // the first real Roblox lifecycle callback.
  jclass roblox_application =
      find_class(jvm->env, "com/roblox/client/RobloxApplication");
  if (!roblox_application) return 0;
  if (trace_enabled()) std::fprintf(stderr, "nuah ART: app-state RobloxApplication class ready\n");
  const char* application_field_name = "b";
  jfieldID application_field = jvm->env->GetStaticFieldID(
      roblox_application, application_field_name,
      "Landroid/content/Context;");
  /* Roblox 2.734 moved the static Application context from b to c while
   * reusing b for a long-lived native/session value. Keep the old contract
   * for the Aug-16 package, but select the newer field by type rather than
   * assuming the obfuscated name is stable. */
  if (!application_field) {
    clear_exception(jvm->env, "RobloxApplication.b lookup");
    application_field_name = "c";
    application_field = jvm->env->GetStaticFieldID(
        roblox_application, application_field_name,
        "Landroid/content/Context;");
    if (application_field && trace_enabled())
      std::fprintf(stderr, "nuah ART: app-state context moved to RobloxApplication.c\n");
  }
  if (!application_field) {
    clear_exception(jvm->env, "RobloxApplication context lookup");
    return 0;
  }
  jvm->env->SetStaticObjectField(roblox_application, application_field,
                                 application);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "RobloxApplication context");
    return 0;
  }
  if (trace_enabled())
    std::fprintf(stderr, "nuah ART: app-state RobloxApplication.%s set\n",
                 application_field_name);

  // Older Roblox builds kept the files/cache/device-id bootstrap in rh.y0
  // (e0/S/r).  That class is obfuscated and its methods move between APK
  // releases; newer builds initialize the same state from their own
  // Application/Activity startup.  Probe the old helpers when present, but
  // never make an optional obfuscated method a hard launch dependency.
  jclass paths = find_class(jvm->env, "rh/y0");
  if (!paths) {
    clear_exception(jvm->env, "rh.y0 class lookup");
    if (trace_enabled())
      std::fprintf(stderr, "nuah ART: optional rh.y0 bootstrap absent\n");
    return 1;
  }
  if (trace_enabled()) std::fprintf(stderr, "nuah ART: app-state %s class ready\n",
                                     "rh.y0");

  jmethodID prepare_paths = jvm->env->GetStaticMethodID(
      paths, "e0", "(Landroid/content/Context;)V");
  /* The current APK moved this exact helper from rh.y0 to rh.z0.  Select it
   * by contract (method signature), not by an obfuscated class name. */
  if (!prepare_paths) {
    clear_exception(jvm->env, "rh.y0.e0 optional lookup");
    jclass moved_paths = find_class(jvm->env, "rh/z0");
    if (moved_paths) {
      const jmethodID moved_prepare = jvm->env->GetStaticMethodID(
          moved_paths, "e0", "(Landroid/content/Context;)V");
      if (moved_prepare) {
        paths = moved_paths;
        prepare_paths = moved_prepare;
        if (trace_enabled())
          std::fprintf(stderr, "nuah ART: app-state helper moved to rh.z0\n");
      } else {
        clear_exception(jvm->env, "rh.z0.e0 optional lookup");
      }
    } else {
      clear_exception(jvm->env, "rh.z0 class lookup");
    }
  }
  if (prepare_paths) {
    jvm->env->CallStaticVoidMethod(paths, prepare_paths, application);
    if (jvm->env->ExceptionCheck())
      clear_exception(jvm->env, "rh.y0.e0");
    else if (trace_enabled())
      std::fprintf(stderr, "nuah ART: app-state rh.y0.e0 done\n");
  } else {
    clear_exception(jvm->env, "rh.y0.e0 optional lookup");
    if (trace_enabled())
      std::fprintf(stderr, "nuah ART: optional rh.y0.e0 absent; continuing\n");
  }

  const jmethodID prefs_factory = jvm->env->GetStaticMethodID(
      paths, "S",
      "(Landroid/content/Context;)Landroid/content/SharedPreferences;");
  if (prefs_factory) {
    jobject prefs =
        jvm->env->CallStaticObjectMethod(paths, prefs_factory, application);
    if (prefs && !jvm->env->ExceptionCheck()) {
      if (trace_enabled()) std::fprintf(stderr, "nuah ART: app-state rh.y0.S done\n");
      const jfieldID prefs_field = jvm->env->GetStaticFieldID(
          paths, "r", "Landroid/content/SharedPreferences;");
      if (prefs_field) {
        jvm->env->SetStaticObjectField(paths, prefs_field, prefs);
        if (jvm->env->ExceptionCheck())
          clear_exception(jvm->env, "rh.y0.r");
        else if (trace_enabled())
          std::fprintf(stderr, "nuah ART: app-state rh.y0.r set\n");
      } else {
        clear_exception(jvm->env, "rh.y0.r optional lookup");
      }
    } else {
      clear_exception(jvm->env, "rh.y0.S optional");
    }
  } else {
    clear_exception(jvm->env, "rh.y0.S optional lookup");
    if (trace_enabled())
      std::fprintf(stderr, "nuah ART: optional rh.y0.S/r absent; continuing\n");

    /* Roblox 2.734 moved the shared-preferences bootstrap to rh.w0.  Its
     * singleton reads rh.w0.r during class initialization, so seed that
     * field from the public T(Context) provider before MainGameActivity
     * constructs the AppManager. */
    jclass moved_preferences = find_class(jvm->env, "rh/w0");
    if (moved_preferences) {
      const jmethodID preferences_provider = jvm->env->GetStaticMethodID(
          moved_preferences, "T",
          "(Landroid/content/Context;)Landroid/content/SharedPreferences;");
      if (preferences_provider) {
        jobject preferences = jvm->env->CallStaticObjectMethod(
            moved_preferences, preferences_provider, application);
        if (preferences && !jvm->env->ExceptionCheck()) {
          const jfieldID moved_preferences_field = jvm->env->GetStaticFieldID(
              moved_preferences, "r",
              "Landroid/content/SharedPreferences;");
          if (moved_preferences_field) {
            jvm->env->SetStaticObjectField(moved_preferences,
                                           moved_preferences_field,
                                           preferences);
            if (jvm->env->ExceptionCheck()) {
              clear_exception(jvm->env, "rh.w0.r");
            } else if (trace_enabled()) {
              std::fprintf(stderr,
                           "nuah ART: app-state rh.w0.T/r initialized\n");
            }
          } else {
            clear_exception(jvm->env, "rh.w0.r lookup");
          }
        } else {
          clear_exception(jvm->env, "rh.w0.T");
        }
      } else {
        clear_exception(jvm->env, "rh.w0.T lookup");
      }
    } else {
      clear_exception(jvm->env, "rh.w0 class lookup");
    }
  }
  return 1;
}

extern "C" int nuah_jvm_dispatch_activity_create(NuahJvm* jvm) {
  if (!jvm || !jvm->env || !jvm->activity) return 0;
  jclass activity_class =
      find_class(jvm->env, "com/roblox/client/startup/MainGameActivity");
  jclass bundle_class = find_class(jvm->env, "android/os/Bundle");
  if (!activity_class || !bundle_class) return 0;
  const jmethodID bundle_ctor =
      jvm->env->GetMethodID(bundle_class, "<init>", "()V");
  const jmethodID on_create =
      jvm->env->GetMethodID(activity_class, "onCreate",
                            "(Landroid/os/Bundle;)V");
  if (!bundle_ctor || !on_create) {
    clear_exception(jvm->env, "MainGameActivity.onCreate lookup");
    return 0;
  }
  jobject bundle = jvm->env->NewObject(bundle_class, bundle_ctor);
  if (!bundle) {
    clear_exception(jvm->env, "MainGameActivity.onCreate Bundle");
    return 0;
  }
  jvm->env->CallVoidMethod(jvm->activity, on_create, bundle);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "MainGameActivity.onCreate");
    return 0;
  }
  return 1;
}

extern "C" int nuah_jvm_capture_native_handle(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return 0;
  jclass activity_class =
      find_class(jvm->env, "com/google/androidgamesdk/GameActivity");
  if (!activity_class) return 0;
  const jfieldID handle = jvm->env->GetStaticFieldID(activity_class, "P", "J");
  if (!handle) {
    clear_exception(jvm->env, "GameActivity native handle lookup");
    return 0;
  }
  const jlong value = jvm->env->GetStaticLongField(activity_class, handle);
  if (jvm->env->ExceptionCheck() || value == 0) {
    clear_exception(jvm->env, "GameActivity native handle read");
    return 0;
  }
  jvm->native_handle = value;
  return 1;
}

extern "C" void* nuah_jvm_key_event(
    NuahJvm* jvm, int keycode, int action, int repeat, int scancode,
    unsigned int modifiers, unsigned long long event_time_ms) {
  if (!jvm) return nullptr;
  jclass klass = find_class(jvm->env, "android/view/KeyEvent");
  if (!klass) return nullptr;
  const char* extended_signature = "(JJIIIIIIII)V";
  jmethodID ctor = jvm->env->GetMethodID(klass, "<init>", extended_signature);
  bool extended = ctor != nullptr;
  if (!ctor) {
    clear_exception(jvm->env, "KeyEvent constructor");
    ctor = jvm->env->GetMethodID(klass, "<init>", "(JJIIII)V");
  }
  if (!ctor) {
    clear_exception(jvm->env, "KeyEvent fallback constructor");
    return nullptr;
  }
  jobject local = nullptr;
  if (extended) {
    local = jvm->env->NewObject(klass, ctor,
                                static_cast<jlong>(event_time_ms),
                                static_cast<jlong>(event_time_ms), action,
                                keycode, repeat, static_cast<jint>(modifiers),
                                0, scancode, 0, 0x101);
  }
  if (!local) {
    clear_exception(jvm->env, "KeyEvent allocation");
    local = jvm->env->NewObject(klass, ctor,
                                static_cast<jlong>(event_time_ms),
                                static_cast<jlong>(event_time_ms), action,
                                keycode, repeat, static_cast<jint>(modifiers));
  }
  return new_global_event(jvm->env, jvm->key_event, local);
}

extern "C" void* nuah_jvm_motion_event(
    NuahJvm* jvm, int action, int /*button*/, double x, double y, double dx,
    double dy, unsigned long long event_time_ms) {
  if (!jvm) return nullptr;
  jclass klass = find_class(jvm->env, "android/view/MotionEvent");
  if (!klass) return nullptr;
  jmethodID ctor = jvm->env->GetMethodID(klass, "<init>", "(IIJFFFF)V");
  if (!ctor) {
    clear_exception(jvm->env, "MotionEvent constructor");
    return nullptr;
  }
  jobject local = jvm->env->NewObject(
      klass, ctor, 0x1002, action, static_cast<jlong>(event_time_ms),
      static_cast<jfloat>(x), static_cast<jfloat>(y),
      static_cast<jfloat>(x + dx), static_cast<jfloat>(y + dy));
  if (!local) {
    clear_exception(jvm->env, "MotionEvent allocation");
    return nullptr;
  }
  return new_global_event(jvm->env, jvm->motion_event, local);
}

extern "C" void* nuah_jvm_surface(NuahJvm* jvm, NuahNativeWindow* window) {
  if (!jvm || !window) return nullptr;

  /* Once MainGameActivity.onCreate has run, GameActivity owns the actual
   * SurfaceView/SurfaceHolder pair.  Use that object for Roblox calls rather
   * than the pre-create façade.  The Java object matters here: Roblox keeps
   * the Surface identity alongside its ANativeWindow state. */
  jobject activity_surface = nullptr;
  if (jvm->activity) {
    jclass game_activity =
        find_class(jvm->env, "com/google/androidgamesdk/GameActivity");
    if (game_activity) {
      const jfieldID holder_field = jvm->env->GetFieldID(
          game_activity, "I", "Landroid/view/SurfaceHolder;");
      jobject holder = holder_field
                           ? jvm->env->GetObjectField(jvm->activity, holder_field)
                           : nullptr;
      /* GameActivity assigns I from surfaceCreated(), which may not have
       * run yet when Nuah is about to start the URI. H is created by
       * GameActivity.onCreate, so obtain the holder from that authoritative
       * SurfaceView as the early-start fallback. */
      if (!holder) {
        clear_exception(jvm->env, "GameActivity holder field");
        const jfieldID view_field = jvm->env->GetFieldID(
            game_activity, "H", "Lcom/google/androidgamesdk/GameActivity$d;");
        jobject view = view_field
                           ? jvm->env->GetObjectField(jvm->activity, view_field)
                           : nullptr;
        if (view) {
          jclass view_class =
              find_class(jvm->env, "android/view/SurfaceView");
          const jmethodID get_holder =
              view_class
                  ? jvm->env->GetMethodID(
                        view_class, "getHolder",
                        "()Landroid/view/SurfaceHolder;")
                  : nullptr;
          if (get_holder)
            holder = jvm->env->CallObjectMethod(view, get_holder);
          clear_exception(jvm->env, "GameActivity SurfaceView.getHolder");
        }
      }
      if (holder) {
        jclass holder_class =
            find_class(jvm->env, "android/view/SurfaceHolder");
        const jmethodID get_surface =
            holder_class
                ? jvm->env->GetMethodID(holder_class, "getSurface",
                                       "()Landroid/view/Surface;")
                : nullptr;
        if (get_surface)
          activity_surface =
              jvm->env->CallObjectMethod(holder, get_surface);
        clear_exception(jvm->env, "GameActivity SurfaceHolder.getSurface");
      }
      clear_exception(jvm->env, "GameActivity SurfaceHolder lookup");
    }
  }

  if (!activity_surface && jvm->surface && jvm->surface_window == window)
    return jvm->surface;
  if (jvm->surface) {
    nuah_native_window_unregister_surface(jvm->surface);
    delete_global(jvm->env, jvm->surface);
    jvm->surface = nullptr;
    jvm->surface_window = nullptr;
  }

  if (activity_surface) {
    jobject global = jvm->env->NewGlobalRef(activity_surface);
    if (!global || !nuah_native_window_alias_surface(window, global)) {
      delete_global(jvm->env, global);
      return nullptr;
    }
    /* Replacing the pre-create façade unregisters its alias, which clears
     * the default entry as a safety measure.  Keep the same host window as
     * the fallback for ART's per-call JNI local-reference tokens. */
    nuah_native_window_set_default(window);
    jvm->surface = global;
    jvm->surface_window = window;
    if (trace_enabled())
      std::fprintf(stderr, "nuah ART: using GameActivity holder Surface %p\n",
                   static_cast<void*>(global));
    return global;
  }

  jclass klass = find_class(jvm->env, "android/view/Surface");
  if (!klass) return nullptr;
  jobject local = jvm->env->AllocObject(klass);
  if (!local) {
    clear_exception(jvm->env, "Surface allocation");
    return nullptr;
  }
  jfieldID widget = jvm->env->GetFieldID(klass, "widget", "J");
  if (widget) jvm->env->SetLongField(local, widget, reinterpret_cast<jlong>(window));
  clear_exception(jvm->env, "Surface widget");
  jobject global = jvm->env->NewGlobalRef(local);
  if (!global || !nuah_native_window_alias_surface(window, global)) {
    delete_global(jvm->env, global);
    return nullptr;
  }
  nuah_native_window_set_default(window);
  jvm->surface = global;
  jvm->surface_window = window;
  return global;
}

extern "C" void nuah_jvm_clear_surface(NuahJvm* jvm) {
  if (!jvm || !jvm->surface) return;
  nuah_native_window_unregister_surface(jvm->surface);
  delete_global(jvm->env, jvm->surface);
  jvm->surface = nullptr;
  jvm->surface_window = nullptr;
}

extern "C" long long nuah_jvm_initialize_game(
    NuahJvm* jvm, const char* /*package_name*/, const char* data_path) {
  if (!jvm || !jvm->activity) return 0;
  jmethodID method = find_instance_method(
      jvm->env, "com/google/androidgamesdk/GameActivity", "initializeNativeCode",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
      "Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J");
  if (!method) return 0;
  jobject assets = make_empty(jvm->env, "android/content/res/AssetManager");
  jobject configuration = make_empty(jvm->env, "android/content/res/Configuration");
  jbyteArray saved = jvm->env->NewByteArray(0);
  const std::string root = data_path ? data_path : "";
  const std::string obb = root + "/obb";
  const std::string external = root + "/external";
  const jlong result = jvm->env->CallLongMethod(
      jvm->activity, method, jvm->env->NewStringUTF(root.c_str()),
      jvm->env->NewStringUTF(obb.c_str()), jvm->env->NewStringUTF(external.c_str()),
      assets, saved, configuration);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "GameActivity.initializeNativeCode");
    return 0;
  }
  jvm->native_handle = result;
  return result;
}

extern "C" int nuah_jvm_dispatch_lifecycle(NuahJvm* jvm,
                                             const char* method_name) {
  if (!jvm || !method_name) return 0;
  jmethodID method = find_instance_method(
      jvm->env, "com/google/androidgamesdk/GameActivity", method_name, "(J)V");
  if (!method) return 0;
  jvm->env->CallVoidMethod(jvm->activity, method, jvm->native_handle);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, method_name);
    return 0;
  }
  return 1;
}

extern "C" int nuah_jvm_dispatch_window_focus(NuahJvm* jvm, int has_focus) {
  if (!jvm || !jvm->activity) return 0;
  /* Use one normal virtual dispatch, exactly like Android's Activity window
   * manager. Do not call ATL's standalone activity_window_ready() here: Nuah
   * is not running ATL's main executable, so that symbol has no activity
   * backlog in this process. Calling both the base and concrete methods also
   * delivers focus twice and can corrupt ART/Roblox renderer state. */
  jmethodID method = find_instance_method(
      jvm->env, "com/google/androidgamesdk/GameActivity",
      "onWindowFocusChanged", "(Z)V");
  if (!method) return 0;
  jvm->env->CallVoidMethod(jvm->activity, method,
                           has_focus ? JNI_TRUE : JNI_FALSE);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "onWindowFocusChanged");
    return 0;
  }
  return 1;
}

extern "C" int nuah_jvm_dispatch_surface_created(NuahJvm* jvm, void* surface) {
  if (!jvm || !surface) return 0;
  jmethodID method = find_instance_method(
      jvm->env, "com/google/androidgamesdk/GameActivity", "onSurfaceCreatedNative",
      "(JLandroid/view/Surface;)V");
  if (!method) return 0;
  jvm->env->CallVoidMethod(jvm->activity, method, jvm->native_handle,
                           static_cast<jobject>(surface));
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "onSurfaceCreatedNative");
    return 0;
  }
  return 1;
}

extern "C" int nuah_jvm_dispatch_surface_changed(
    NuahJvm* jvm, void* surface, int format, int width, int height) {
  if (!jvm || !surface || width <= 0 || height <= 0) return 0;
  jmethodID method = find_instance_method(
      jvm->env, "com/google/androidgamesdk/GameActivity", "onSurfaceChangedNative",
      "(JLandroid/view/Surface;III)V");
  if (!method) return 0;
  jvm->env->CallVoidMethod(jvm->activity, method, jvm->native_handle,
                           static_cast<jobject>(surface), format, width, height);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "onSurfaceChangedNative");
    return 0;
  }
  return 1;
}

extern "C" int nuah_jvm_dispatch_surface_view_lifecycle(
    NuahJvm* jvm, int width, int height) {
  if (!jvm || !jvm->activity || width <= 0 || height <= 0) return 0;
  jclass game_activity =
      find_class(jvm->env, "com/google/androidgamesdk/GameActivity");
  if (!game_activity) return 0;
  const jfieldID view_field = jvm->env->GetFieldID(
      game_activity, "H", "Lcom/google/androidgamesdk/GameActivity$d;");
  jobject view = view_field
                     ? jvm->env->GetObjectField(jvm->activity, view_field)
                     : nullptr;
  clear_exception(jvm->env, "GameActivity SurfaceView field");
  if (!view) return 0;
  jclass surface_view = find_class(jvm->env, "android/view/SurfaceView");
  const jmethodID dispatch = surface_view
                                 ? jvm->env->GetMethodID(
                                       surface_view, "dispatchSurfaceLifecycle",
                                       "(II)V")
                                 : nullptr;
  const char* direct = std::getenv("NUAH_SURFACE_VIEW_CALLBACK_DIRECT");
  if (direct && *direct && std::strcmp(direct, "0") != 0 && surface_view) {
    /* The installed ATL Java façade can enqueue View.post(), but its UI
     * queue is not guaranteed to drain while the native launch call is
     * synchronously joining a room.  For this diagnostic path invoke the
     * same private SurfaceView methods immediately, after Roblox has
     * registered its SurfaceHolder callback.  This is intentionally opt-in;
     * normal launches retain Android's queued ordering. */
    const jmethodID created = jvm->env->GetMethodID(
        surface_view, "surfaceCreated", "()V");
    const jmethodID changed = jvm->env->GetMethodID(
        surface_view, "surfaceChanged", "(III)V");
    if (created && changed) {
      jvm->env->CallVoidMethod(view, created);
      jvm->env->CallVoidMethod(view, changed, 1, width, height);
      if (jvm->env->ExceptionCheck()) {
        clear_exception(jvm->env, "SurfaceView direct lifecycle call");
        return 0;
      }
      if (trace_enabled()) {
        std::fprintf(stderr,
                     "nuah ART: dispatched SurfaceView lifecycle directly "
                     "%dx%d\n",
                     width, height);
      }
      return 1;
    }
    clear_exception(jvm->env, "SurfaceView direct lifecycle lookup");
  }
  if (!dispatch) {
    clear_exception(jvm->env, "SurfaceView.dispatchSurfaceLifecycle");
    return 0;
  }
  jvm->env->CallVoidMethod(view, dispatch, width, height);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "SurfaceView.dispatchSurfaceLifecycle call");
    return 0;
  }
  if (trace_enabled()) {
    std::fprintf(stderr,
                 "nuah ART: queued SurfaceView lifecycle %dx%d\n", width,
                 height);
  }
  return 1;
}

extern "C" int nuah_jvm_dispatch_surface_destroyed(NuahJvm* jvm, void* surface) {
  (void)surface;
  if (!jvm) return 0;
  jmethodID method = find_instance_method(
      jvm->env, "com/google/androidgamesdk/GameActivity", "onSurfaceDestroyedNative",
      "(J)V");
  if (!method) return 0;
  jvm->env->CallVoidMethod(jvm->activity, method, jvm->native_handle);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "onSurfaceDestroyedNative");
    return 0;
  }
  return 1;
}

extern "C" int nuah_jvm_dispatch_key(
    NuahJvm* jvm, int keycode, int action, int repeat, int scancode,
    unsigned int modifiers, unsigned long long event_time_ms) {
  if (!jvm) return 0;
  // Nuah's SDL bridge uses 1=down and 0=up. Android KeyEvent uses the
  // opposite numeric values (ACTION_DOWN=0, ACTION_UP=1).
  const bool key_down = action != 0;
  const char* activity_name = key_down ? "onKeyDown" : "onKeyUp";
  const char* native_name = key_down ? "onKeyDownNative" : "onKeyUpNative";
  const int android_action = key_down ? 0 : 1;
  if (input_trace_enabled()) {
    std::fprintf(stderr,
                 "nuah input: keycode=%d action=%s android_action=%d "
                 "scancode=%d repeat=%d modifiers=0x%x\n",
                 keycode, activity_name, android_action, scancode, repeat,
                 modifiers);
  }

  /* Sober does not inject directly into GameActivity's native method. Its
   * Android path sends the event through MainGameActivity.onKeyDown/Up. That
   * override handles the small set of system keys itself and delegates
   * ordinary keyboard input to GameActivity, which then calls Roblox's
   * registered onKeyDownNative/onKeyUpNative callback. Keeping this Java
   * boundary is important: it preserves the app's filtering, repeat and
   * focus semantics instead of only making a callback-shaped JNI call. */
  jmethodID activity_method = find_instance_method(
      jvm->env, "com/roblox/client/startup/MainGameActivity", activity_name,
      "(ILandroid/view/KeyEvent;)Z");
  jmethodID method = find_instance_method(
      jvm->env, "com/google/androidgamesdk/GameActivity", native_name,
      "(JLandroid/view/KeyEvent;)Z");
  jobject event = static_cast<jobject>(nuah_jvm_key_event(
      jvm, keycode, android_action, repeat, scancode, modifiers,
      event_time_ms));
  if (!event) return 0;

  /* This is the actual Sober desktop-keyboard contract, recovered from the
   * APK's kl.g helper:
   *   nativePassKeyEvent(down, event.getScanCode(), event.getKeyCode(), repeat)
   * The first integer is the raw Android/evdev scan code, not the logical
   * Android key code. */
  if (is_gameplay_key(keycode) && ensure_native_gl_method(jvm)) {
    jvm->env->CallStaticVoidMethod(
        jvm->native_gl_interface, jvm->native_pass_key_event,
        key_down ? JNI_TRUE : JNI_FALSE, static_cast<jint>(scancode),
        static_cast<jint>(keycode), repeat ? JNI_TRUE : JNI_FALSE);
    if (!jvm->env->ExceptionCheck()) {
      if (input_trace_enabled())
        std::fprintf(stderr,
                     "nuah input: Roblox key dispatched keycode=%d "
                     "scancode=%d down=%d repeat=%d\n",
                     keycode, scancode, key_down ? 1 : 0, repeat ? 1 : 0);
      return 1;
    }
    clear_exception(jvm->env, "NativeGLInterface gameplay key");
  }

  if (activity_method) {
    const jboolean result = jvm->env->CallBooleanMethod(
        jvm->activity, activity_method, keycode, event);
    if (!jvm->env->ExceptionCheck()) {
      if (input_trace_enabled())
        std::fprintf(stderr, "nuah input: %s result=%d\n", activity_name,
                     result == JNI_TRUE ? 1 : 0);
      if (result == JNI_TRUE) return 1;
    }
    // A missing optional NativeGLInterface binding should not permanently
    // drop input. Clear it and use the registered GameActivity callback as a
    // diagnostic fallback; normal keys never take this branch.
    clear_exception(jvm->env, activity_name);
  }

  /* Sober's hardware-keyboard branch calls this static native directly from
   * MainGameActivity.  If the façade's Configuration did not make that
   * branch report handled=true, use the same exact Roblox method rather than
   * dropping W/A/S/D and number keys at the generic GameActivity fallback. */
  if (ensure_native_gl_method(jvm)) {
    jvm->env->CallStaticVoidMethod(
        jvm->native_gl_interface, jvm->native_pass_key_event,
        key_down ? JNI_TRUE : JNI_FALSE, static_cast<jint>(scancode),
        static_cast<jint>(keycode), repeat ? JNI_TRUE : JNI_FALSE);
    if (!jvm->env->ExceptionCheck()) {
      if (input_trace_enabled())
        std::fprintf(stderr, "nuah input: NativeGLInterface.nativePassKeyEvent direct\n");
      return 1;
    }
    clear_exception(jvm->env, "NativeGLInterface.nativePassKeyEvent");
  }

  if (!method) return 0;
  const jboolean result = jvm->env->CallBooleanMethod(
      jvm->activity, method, jvm->native_handle, event);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, native_name);
    return 0;
  }
  if (input_trace_enabled())
    std::fprintf(stderr, "nuah input: %s fallback result=%d\n", native_name,
                 result == JNI_TRUE ? 1 : 0);
  return result == JNI_TRUE;
}

extern "C" int nuah_jvm_dispatch_pointer(
    NuahJvm* jvm, int pointer_type, int action, int button, double x,
    double y, double dx, double dy, unsigned long long event_time_ms) {
  if (!jvm) return 0;

  /* This is the path used by Sober's il.e adapter for a desktop mouse.  The
   * previous Nuah implementation converted every pointer event into a
   * touchscreen MotionEvent, which leaves Roblox's PC camera/input layer
   * untouched.  Call the APK's own NativeInputInterface methods first and
   * retain the MotionEvent route only as a compatibility fallback. */
  if (ensure_native_input_methods(jvm)) {
    /* input_bridge.cpp already queries Roblox's mouse-lock callback before
     * each motion/wheel event and synchronizes SDL relative mode from that
     * result.  Calling the same Java method again here duplicated the hot JNI
     * boundary (and the engine's FunctionMarshal work) for every pointer
     * sample.  Button events do not need a fresh lock query: capture is
     * changed only by the synchronizer, while this function only forwards the
     * edge to Roblox. */
    bool dispatched = false;
    if (pointer_type == NUAH_POINTER_MOTION &&
        jvm->native_pass_mouse_move) {
      jvm->env->CallStaticVoidMethod(
          jvm->native_input_interface, jvm->native_pass_mouse_move,
          static_cast<jfloat>(x), static_cast<jfloat>(y),
          static_cast<jfloat>(dx), static_cast<jfloat>(dy));
      dispatched = true;
    } else if (pointer_type == NUAH_POINTER_BUTTON &&
               jvm->native_pass_mouse_button) {
      /* Sober passes MotionEvent.actionButton - 1. SDL numbers buttons as
       * 1=left, 2=middle, 3=right, while Android uses button bit values
       * 1=primary, 2=secondary, 4=tertiary. Therefore right is native 1
       * (not SDL 3 - 1 == 2), and middle is native 3. */
      jint native_button = 0;
      switch (button) {
        case 1: native_button = 0; break;  // Android BUTTON_PRIMARY - 1
        case 3: native_button = 1; break;  // BUTTON_SECONDARY - 1
        case 2: native_button = 3; break;  // BUTTON_TERTIARY - 1
        case 4: native_button = 7; break;  // BUTTON_BACK - 1
        case 5: native_button = 15; break; // BUTTON_FORWARD - 1
        default: native_button = button > 0 ? button - 1 : 0; break;
      }
      jvm->env->CallStaticVoidMethod(
          jvm->native_input_interface, jvm->native_pass_mouse_button,
          static_cast<jfloat>(x), static_cast<jfloat>(y),
          action != 0 ? JNI_TRUE : JNI_FALSE, native_button);
      dispatched = true;
    } else if (pointer_type == NUAH_POINTER_WHEEL &&
               jvm->native_pass_mouse_wheel) {
      /* SDL reports vertical wheel movement in dy, matching Android's
       * AXIS_VSCROLL value used by Sober. Preserve horizontal wheel input if
       * the vertical axis is zero. */
      const double amount = dy != 0.0 ? dy : dx;
      jvm->env->CallStaticVoidMethod(
          jvm->native_input_interface, jvm->native_pass_mouse_wheel,
          static_cast<jfloat>(x), static_cast<jfloat>(y),
          static_cast<jfloat>(amount));
      dispatched = true;
    }
    if (dispatched) {
      if (jvm->env->ExceptionCheck()) {
        clear_exception(jvm->env, "NativeInputInterface mouse dispatch");
      } else {
        if (input_trace_enabled()) {
          std::fprintf(stderr,
                       "nuah input: mouse type=%d action=%d button=%d "
                       "x=%.2f y=%.2f dx=%.2f dy=%.2f\n",
                       pointer_type, action, button, x, y, dx, dy);
        }
        return 1;
      }
    }
  }

  jmethodID method = find_instance_method(
      jvm->env, "com/google/androidgamesdk/GameActivity", "onTouchEventNative",
      "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z");
  jobject event = static_cast<jobject>(nuah_jvm_motion_event(
      jvm, action, button, x, y, dx, dy, event_time_ms));
  if (!method || !event) return 0;
  /* GameActivity's native contract mirrors MotionEvent.dispatch: the five
   * integer fields are pointerCount, historySize, deviceId, source, action.
   * Passing our action/button pair in the first two slots made GameActivity
   * believe a mouse move had two pointers and it called getPointerId(1) on a
   * one-pointer façade, aborting ART. */
  constexpr jint kPointerCount = 1;
  constexpr jint kHistorySize = 0;
  constexpr jint kDeviceId = 0;
  constexpr jint kSource = 0x1002;  // SOURCE_TOUCHSCREEN, ATL's pointer path
  const jint action_button = action == 0 || action == 1 ? button : 0;
  const jint button_state = button;
  const jboolean result = jvm->env->CallBooleanMethod(
      jvm->activity, method, jvm->native_handle, event, kPointerCount,
      kHistorySize, kDeviceId, kSource, action,
      static_cast<jlong>(event_time_ms), static_cast<jlong>(event_time_ms),
      0, 0, action_button, button_state, 0, 0, 1.0f, 1.0f);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "onTouchEventNative");
    return 0;
  }
  return result == JNI_TRUE;
}

extern "C" int nuah_jvm_is_mouse_locked_center(NuahJvm* jvm) {
  if (!jvm || !ensure_native_input_methods(jvm) ||
      !jvm->native_get_mouse_locked_center)
    return -1;
  const jboolean locked = jvm->env->CallStaticBooleanMethod(
      jvm->native_input_interface, jvm->native_get_mouse_locked_center);
  if (jvm->env->ExceptionCheck()) {
    clear_exception(jvm->env, "NativeInputInterface mouse-lock query");
    return -1;
  }
  return locked == JNI_TRUE ? 1 : 0;
}

extern "C" int nuah_jvm_dispatch_motion(
    NuahJvm* jvm, int action, int button, double x, double y, double dx,
    double dy, unsigned long long event_time_ms) {
  return nuah_jvm_dispatch_pointer(jvm, NUAH_POINTER_MOTION, action, button,
                                   x, y, dx, dy, event_time_ms);
}
