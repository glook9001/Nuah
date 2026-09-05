#include <dlfcn.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "nuah/input_bridge.h"
#include "nuah/native_window_bridge.h"
#include "nuah/nuah_jvm.h"
using NativeDlsym = void* (*)(void*, const char*);
using NativeDlopen = void* (*)(const char*, int);
struct NuahJvm {
  JavaVM* vm = nullptr; JNIEnv* env = nullptr;
  pthread_t vm_owner{}; bool vm_owner_attached = false; void* art = nullptr; void* api_native = nullptr;
  NativeDlsym api_dlsym = nullptr;
  jobject activity = nullptr; jobject surface = nullptr; jobject key_event = nullptr; jobject motion_event = nullptr;
  jclass key_event_class = nullptr; jmethodID key_event_ctor = nullptr; bool key_event_extended = false; bool key_event_lookup_done = false;
  jclass motion_event_class = nullptr; jmethodID motion_event_ctor = nullptr; bool motion_event_lookup_done = false;
  jclass native_input_interface = nullptr; jmethodID native_pass_mouse_move = nullptr; jmethodID native_pass_mouse_button = nullptr;
  jmethodID native_pass_mouse_wheel = nullptr; jmethodID native_get_mouse_locked_center = nullptr; bool native_input_lookup_attempted = false;
  jclass native_gl_interface = nullptr; jmethodID native_pass_key_event = nullptr; bool native_gl_lookup_attempted = false;
  jlong native_handle = 0; NuahNativeWindow* surface_window = nullptr; std::string class_path;
};
extern "C" {
void* window; char* apk_path;
void* main_thread_id = nullptr;
}
extern "C" char* get_app_data_dir() {
  static std::string path;
  if (path.empty()) { if (const char* value = std::getenv("ANDROID_APP_DATA_DIR"); value && *value) path = value; else path = "/tmp/nuah"; }
  return path.data();
}
extern "C" void back_button_set_sensitive(bool) {}
extern "C" jint nuah_art_log_println(JNIEnv* env, jclass, jint, jint, jstring, jstring message) {
  static const bool android_log_enabled = [] {
    const char* v = std::getenv("NUAH_ANDROID_LOG");
    return v && *v && std::strcmp(v, "0") != 0;
  }();
  if (android_log_enabled && env && message) {
    const char* text = env->GetStringUTFChars(message, nullptr);
    if (text) { std::fprintf(stderr, "[android] %s\n", text); env->ReleaseStringUTFChars(message, text); }
  }
  return 0;
}
extern "C" void nuah_art_logging_protocol_native_log_event(JNIEnv*, jclass, jstring, jlong, jobjectArray) {}
extern "C" void nuah_art_context_update_config(JNIEnv*, jclass, jobject) {
}
namespace {
using CreateJavaVm = jint (*)(JavaVM**, void**, void*);
using AtlJniSetup = void (*)(JNIEnv*);
using GtkInitCheck = int (*)();
bool trace_enabled();
inline const char* env_nonempty(const char* n) { const char* v = std::getenv(n); return v && *v ? v : nullptr; }
inline bool env_flag(const char* n) { const char* v = env_nonempty(n); return v && std::strcmp(v, "0") != 0; }
inline std::vector<std::string> split_colon(const char* value) {
  std::vector<std::string> out;
  if (!value || !*value) return out;
  std::string all(value);
  std::size_t begin = 0;
  while (begin <= all.size()) {
    const std::size_t end = all.find(':', begin);
    if (end == std::string::npos) { if (begin < all.size()) out.push_back(all.substr(begin)); break; }
    if (end != begin) out.push_back(all.substr(begin, end - begin));
    begin = end + 1;
  }
  return out;
}
template <typename... Args>
inline void art_trace(const char* fmt, Args&&... args) {
  if (trace_enabled()) std::fprintf(stderr, fmt, std::forward<Args>(args)...);
}
bool is_gameplay_key(int keycode) {
  /* Roblox's desktop input layer consumes these through NativeGLInterface.nativePassKeyEvent ("main" thread proof). MainGameActivity.onKeyDown can report handled. */
  return (keycode >= NUAH_KEY_0 && keycode <= NUAH_KEY_9) || (keycode >= NUAH_KEY_DPAD_UP && keycode <= NUAH_KEY_DPAD_RIGHT) || (keycode >= NUAH_KEY_A && keycode <= NUAH_KEY_Z) ||
         keycode == NUAH_KEY_TAB || keycode == NUAH_KEY_SPACE || keycode == NUAH_KEY_ENTER || keycode == NUAH_KEY_SHIFT_LEFT || keycode == NUAH_KEY_SHIFT_RIGHT || keycode == NUAH_KEY_CTRL_LEFT ||
         keycode == NUAH_KEY_CTRL_RIGHT || keycode == NUAH_KEY_ALT_LEFT || keycode == NUAH_KEY_ALT_RIGHT || keycode == NUAH_KEY_ESCAPE;
}
void* api_symbol(const NuahJvm* jvm, const char* symbol) {
  if (!jvm || !jvm->api_native || !symbol) return nullptr;
  return jvm->api_dlsym ? jvm->api_dlsym(jvm->api_native, symbol) : ::dlsym(jvm->api_native, symbol);
}
void initialize_atl_host_state(NuahJvm* jvm) {
  if (!jvm || !jvm->env || !jvm->api_native) return;
  if (auto* vm_slot = reinterpret_cast<JavaVM**>(api_symbol(jvm, "jvm"))) { *vm_slot = jvm->vm; }
  if (auto setup = reinterpret_cast<AtlJniSetup>(api_symbol(jvm, "set_up_handle_cache"))) {
    setup(jvm->env);
  } else art_trace("nuah ART: ATL handle-cache setup export missing\n");
  auto gtk_init = reinterpret_cast<GtkInitCheck>(::dlsym(RTLD_DEFAULT, "gtk_init_check"));
  if (gtk_init && !gtk_init()) { art_trace("nuah ART: GTK display initialization failed\n"); }
}
bool trace_enabled() { return env_flag("NUAH_BOOTSTRAP_TRACE"); }
bool input_trace_enabled() { return env_flag("NUAH_INPUT_TRACE"); }
void clear_exception(JNIEnv* env, const char* boundary) {
  if (!env || !env->ExceptionCheck()) return;
  if (trace_enabled()) { art_trace("nuah ART: Java exception at %s\n", boundary); env->ExceptionDescribe(); }
  env->ExceptionClear();
}
inline bool check_clear(JNIEnv* env, const char* boundary) {
  if (env && env->ExceptionCheck()) { clear_exception(env, boundary); return true; }
  return false;
}
template <typename... Args>
inline void input_trace(const char* fmt, Args&&... args) {
  if (input_trace_enabled()) std::fprintf(stderr, fmt, std::forward<Args>(args)...);
}
#define NUAH_GAME_METHOD(env_, name_, sig_) find_instance_method(env_, "com/google/androidgamesdk/GameActivity", name_, sig_)
#define NUAH_ROBLOX_METHOD(env_, name_, sig_) find_instance_method(env_, "com/roblox/client/startup/MainGameActivity", name_, sig_)
jclass find_class(JNIEnv* env, const char* name) {
  if (!env || !name) return nullptr;
  jclass klass = env->FindClass(name);
  if (!klass) clear_exception(env, name);
  return klass;
}
std::string artifact_root() {
  if (const char* value = env_nonempty("NUAH_ART_HOME")) return value;
  return "/usr/local/lib64/java/dex/android_translation_layer";
}
std::string art_path() {
  if (const char* value = env_nonempty("NUAH_ART_LIBRARY")) return value;
  if (const char* directory = env_nonempty("NUAH_ART_LIBRARY_DIR")) return (std::filesystem::path(directory) / "libart.so").string();
  return "/usr/local/lib64/art/libart.so";
}
std::vector<std::string> apk_class_paths() {
  std::vector<std::string> paths;
  for (const auto& path : split_colon(std::getenv("NUAH_APK_PATHS"))) {
    if (std::filesystem::is_regular_file(path)) paths.push_back(path);
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
/* The host ART image is tied to the exact boot-classpath order used when it was compiled. Native-run used to leave the inherited (and often. */
std::string android16_bootclasspath() {
  static constexpr const char* kOrder[] = {
      "core-oj-hostdex.jar",     "apachehttp-hostdex.jar", "apache-xml-hostdex.jar",   "bouncycastle-hostdex.jar", "core-junit-hostdex.jar",
      "core-libart-hostdex.jar", "hamcrest-hostdex.jar",   "junit-runner-hostdex.jar", "okhttp-hostdex.jar",       "wolfssljni-hostdex.jar",
  };
  std::vector<std::filesystem::path> entries;
  for (const auto& token : split_colon(std::getenv("NUAH_ATL_ANDROID16_BOOTCLASSPATH"))) {
    std::filesystem::path path(token);
    if (path.is_relative()) { if (const char* home = env_nonempty("NUAH_ATL_ANDROID16_HOME")) path = std::filesystem::path(home) / path; }
    entries.push_back(std::move(path));
  }
  if (entries.empty()) { const std::filesystem::path root = "/usr/local/lib64/java/dex/art"; for (const char* basename : kOrder) entries.emplace_back(root / basename); }
  std::vector<std::filesystem::path> ordered;
  std::vector<bool> used(entries.size(), false);
  ordered.reserve(entries.size());
  for (const char* basename : kOrder) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
      if (!used[index] && entries[index].filename() == basename) { ordered.push_back(entries[index]); used[index] = true; }
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
void delete_global(JNIEnv* env, jobject& object) { if (env && object) env->DeleteGlobalRef(object); object = nullptr; }
void delete_global_class(JNIEnv* env, jclass& klass) { if (env && klass) env->DeleteGlobalRef(klass); klass = nullptr; }
inline bool ensure_global_class(JNIEnv* env, jclass& slot, const char* name);
jmethodID find_static_method(JNIEnv* env, jclass klass, const char* name, const char* signature) {
  if (!env || !klass) return nullptr;
  jmethodID method = env->GetStaticMethodID(klass, name, signature);
  if (!method) clear_exception(env, name);
  return method;
}
bool ensure_native_input_methods(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return false;
  if (jvm->native_input_lookup_attempted) return jvm->native_input_interface != nullptr;
  jvm->native_input_lookup_attempted = true;
  if (!ensure_global_class(jvm->env, jvm->native_input_interface, "com/roblox/engine/jni/NativeInputInterface")) return false;
  jvm->native_pass_mouse_move = find_static_method(jvm->env, jvm->native_input_interface, "nativePassMouseMove", "(FFFF)V");
  jvm->native_pass_mouse_button = find_static_method(jvm->env, jvm->native_input_interface, "nativePassMouseButton", "(FFZI)V");
  jvm->native_pass_mouse_wheel = find_static_method(jvm->env, jvm->native_input_interface, "nativePassMouseWheel", "(FFF)V");
  jvm->native_get_mouse_locked_center = find_static_method(jvm->env, jvm->native_input_interface, "nativeGetMainWindowIsMouseLockedCenter", "()Z");
  input_trace("nuah input: NativeInputInterface move=%p button=%p wheel=%p locked=%p\n", reinterpret_cast<void*>(jvm->native_pass_mouse_move),
              reinterpret_cast<void*>(jvm->native_pass_mouse_button), reinterpret_cast<void*>(jvm->native_pass_mouse_wheel), reinterpret_cast<void*>(jvm->native_get_mouse_locked_center));
  return jvm->native_pass_mouse_move || jvm->native_pass_mouse_button || jvm->native_pass_mouse_wheel;
}
bool ensure_native_gl_method(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return false;
  if (jvm->native_gl_lookup_attempted) return jvm->native_gl_interface != nullptr && jvm->native_pass_key_event != nullptr;
  jvm->native_gl_lookup_attempted = true;
  if (!ensure_global_class(jvm->env, jvm->native_gl_interface, "com/roblox/engine/jni/NativeGLInterface")) return false;
  jvm->native_pass_key_event = find_static_method(jvm->env, jvm->native_gl_interface, "nativePassKeyEvent", "(ZIIZ)V");
  return jvm->native_pass_key_event != nullptr;
}
inline bool pass_key_to_engine(NuahJvm* jvm, bool down, jint scan, jint code, jboolean repeat, const char* boundary) {
  if (!ensure_native_gl_method(jvm)) return false;
  jvm->env->CallStaticVoidMethod(jvm->native_gl_interface, jvm->native_pass_key_event, down ? JNI_TRUE : JNI_FALSE, scan, code, repeat);
  if (check_clear(jvm->env, boundary)) return false;
  return true;
}
inline jint sdl_button_to_native(int button) {
  constexpr jint kMap[][2] = {{1, 0}, {3, 1}, {2, 3}, {4, 7}, {5, 15}};
  for (const auto& entry : kMap)
    if (entry[0] == button) return entry[1];
  return button > 0 ? button - 1 : 0;
}
jobject make_empty(JNIEnv* env, const char* class_name) {
  jclass klass = find_class(env, class_name);
  if (!klass) return nullptr;
  jmethodID ctor = env->GetMethodID(klass, "<init>", "()V");
  if (!ctor) { clear_exception(env, class_name); return env->AllocObject(klass); }
  jobject result = env->NewObject(klass, ctor);
  clear_exception(env, class_name);
  return result;
}
jobject new_global_event(JNIEnv* env, jobject& slot, jobject local) { delete_global(env, slot); if (!local) return nullptr; slot = env->NewGlobalRef(local); return slot; }
jmethodID find_instance_method(JNIEnv* env, const char* klass_name, const char* method, const char* signature) {
  jclass klass = find_class(env, klass_name);
  if (!klass) return nullptr;
  jmethodID result = env->GetMethodID(klass, method, signature);
  if (!result) clear_exception(env, method);
  return result;
}
bool register_host_native(JNIEnv* env, const char* klass_name, const char* method_name, const char* signature, void* function) {
  jclass klass = find_class(env, klass_name);
  if (!klass || !function) return false;
  const JNINativeMethod method{method_name, signature, function};
  const jint result = env->RegisterNatives(klass, &method, 1);
  if (result != JNI_OK) clear_exception(env, method_name);
  return result == JNI_OK;
}
// Table-driven GameActivity void dispatch: lookup (same class/sig strings) + call + check.
template <typename Call>
inline int dispatch_game_void(NuahJvm* jvm, const char* name, const char* sig, const char* boundary, Call&& call) {
  if (!jvm) return 0;
  jmethodID method = find_instance_method(jvm->env, "com/google/androidgamesdk/GameActivity", name, sig);
  if (!method) return 0;
  call(method);
  if (check_clear(jvm->env, boundary ? boundary : name)) return 0;
  return 1;
}
// Dedupe event-class global-ref boilerplate (KeyEvent/MotionEvent share FindClass + NewGlobalRef).
inline bool ensure_global_class(JNIEnv* env, jclass& slot, const char* name) {
  if (slot) return true;
  jclass local = find_class(env, name);
  if (!local) return false;
  slot = reinterpret_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  return slot != nullptr;
}
inline jobject store_surface(NuahJvm* jvm, NuahNativeWindow* window, jobject global) {
  if (!global || !nuah_native_window_alias_surface(window, global)) { delete_global(jvm->env, global); return nullptr; }
  nuah_native_window_set_default(window);
  jvm->surface = global; jvm->surface_window = window;
  return global;
}
struct AtlNativeSpec {
  const char* klass;
  const char* method;
  const char* signature;
  const char* symbol;
};
void register_atl_natives(JNIEnv* env, void* handle, NativeDlsym lookup, const AtlNativeSpec* specs, std::size_t count) {
  if (!env || !handle || !specs) return;
  for (std::size_t index = 0; index < count; ++index) {
    const auto& spec = specs[index];
    void* function = lookup ? lookup(handle, spec.symbol) : dlsym(handle, spec.symbol);
    if (!function) art_trace("nuah ART: ATL export missing %s\n", spec.symbol);
    (void)register_host_native(env, spec.klass, spec.method, spec.signature, function);
  }
}
bool load_atl_library_for_classloader(JNIEnv* env) {
  if (!env) return false;
  jclass view = find_class(env, "android/view/View");
  jclass runtime_class = find_class(env, "java/lang/Runtime");
  if (!view || !runtime_class) return false;
  const jmethodID get_loader = env->GetMethodID(env->GetObjectClass(view), "getClassLoader", "()Ljava/lang/ClassLoader;"); const jmethodID get_runtime = env->GetStaticMethodID(runtime_class, "getRuntime", "()Ljava/lang/Runtime;");
  const jmethodID load_library = env->GetMethodID(runtime_class, "loadLibrary", "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
  if (!get_loader || !get_runtime || !load_library) { clear_exception(env, "ATL Runtime.loadLibrary lookup"); return false; }
  jobject loader = env->CallObjectMethod(view, get_loader);
  jobject runtime = env->CallStaticObjectMethod(runtime_class, get_runtime);
  jstring name = env->NewStringUTF("translation_layer_main");
  env->CallVoidMethod(runtime, load_library, name, loader);
  env->DeleteLocalRef(name);
  if (env->ExceptionCheck()) { art_trace("nuah ART: ATL Runtime.loadLibrary failed\n"); clear_exception(env, "ATL Runtime.loadLibrary"); return false; }
  return true;
}
bool initialize_roblox_device_static_params(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return false;
  JNIEnv* env = jvm->env;
  jclass bridge = find_class(env, "com/roblox/engine/jni/NativeGLJavaInterface");
  jclass params_class = find_class(env, "com/roblox/engine/jni/model/DeviceStaticParams");
  if (!bridge || !params_class) return false;
  const jmethodID ctor = env->GetMethodID(params_class, "<init>", "()V");
  const jmethodID setter = env->GetStaticMethodID(bridge, "setDeviceStaticParams", "(Lcom/roblox/engine/jni/model/DeviceStaticParams;)V");
  if (!ctor || !setter) { clear_exception(env, "DeviceStaticParams setup methods"); return false; }
  jobject params = env->NewObject(params_class, ctor);
  if (!params) { clear_exception(env, "DeviceStaticParams setup object"); return false; }
  static constexpr struct { const char* f; const char* v; } kStrings[] = {
      {"appBuildVariant", "release"}, {"appVersion", "Roblox"}, {"deviceName", "Nuah Linux PC"}, {"deviceSku", "x86_64"}, {"manufacturer", "Nuah"}, {"osVersion", "36"}, {"socModel", "x86_64"}, };
  const jfieldID cpu = env->GetFieldID(params_class, "cpu64Bit", "Z");
  bool ok = cpu != nullptr;
  for (const auto& e : kStrings) {
    const jfieldID f = ok ? env->GetFieldID(params_class, e.f, "Ljava/lang/String;") : nullptr;
    if (!f) { ok = false; break; }
    const jstring t = env->NewStringUTF(e.v); env->SetObjectField(params, f, t); env->DeleteLocalRef(t);
    if (env->ExceptionCheck()) { ok = false; break; }
  }
  if (!ok || env->ExceptionCheck()) { clear_exception(env, "DeviceStaticParams setup fields"); return false; }
  env->SetBooleanField(params, cpu, JNI_TRUE);
  env->CallStaticVoidMethod(bridge, setter, params);
  if (check_clear(env, "DeviceStaticParams setup setter")) return false;
  return true;
}
void force_android_sdk_level(JNIEnv* env, jint sdk) {
  if (!env) return;
  jclass system = find_class(env, "java/lang/System");
  if (system) {
    const jmethodID set_property = env->GetStaticMethodID(system, "setProperty", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (set_property) {
      const jstring key = env->NewStringUTF("Build.VERSION.SDK_INT"); const jstring value = env->NewStringUTF("36");
      env->CallStaticObjectMethod(system, set_property, key, value);
      env->DeleteLocalRef(key); env->DeleteLocalRef(value);
      clear_exception(env, "System.setProperty(Build.VERSION.SDK_INT)");
    } else clear_exception(env, "System.setProperty lookup");
  }
  jclass version = find_class(env, "android/os/Build$VERSION");
  if (!version) return;
  static constexpr struct { const char* f; const char* b; } kSdkFields[] = { {"SDK_INT", "Build.VERSION.SDK_INT"}, {"RESOURCES_SDK_INT", "Build.VERSION.RESOURCES_SDK_INT"}, };
  for (const auto& e : kSdkFields) { const jfieldID f = env->GetStaticFieldID(version, e.f, "I"); if (f) env->SetStaticIntField(version, f, sdk); clear_exception(env, e.b); }
  const jfieldID sdk_string = env->GetStaticFieldID(version, "SDK", "Ljava/lang/String;");
  if (sdk_string) { const jstring value = env->NewStringUTF("36"); env->SetStaticObjectField(version, sdk_string, value); env->DeleteLocalRef(value); }
  clear_exception(env, "Build.VERSION.SDK");
  art_trace("nuah ART: forced Android SDK level %d\n", static_cast<int>(sdk));
}
void bind_host_r_debug() {
  /* ART's libart is DT_NEEDED on libdl_bio. That linker stores a host r_debug pointer; leaving it null makes apkenv_find_library fault at. */
  void* bio = ::dlopen("libdl_bio.so.0", RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
  if (!bio) bio = ::dlopen("libdl_bio.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!bio) return;
  auto** linker_debug = reinterpret_cast<struct r_debug**>(::dlsym(bio, "_r_debug_ptr"));
  if (linker_debug && !*linker_debug) {
    if (auto* host = reinterpret_cast<struct r_debug*>(::dlsym(RTLD_DEFAULT, "_r_debug"))) *linker_debug = host;
  }
}
}  // namespace
extern "C" NuahJvm* nuah_jvm_create(void) {
  auto* jvm = new NuahJvm;
  jvm->art = dlopen(art_path().c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!jvm->art) { std::fprintf(stderr, "nuah ART: cannot load %s: %s\n", art_path().c_str(), dlerror()); delete jvm; return nullptr; }
  bind_host_r_debug();
  auto create = reinterpret_cast<CreateJavaVm>(dlsym(jvm->art, "JNI_CreateJavaVM"));
  if (!create) { std::fprintf(stderr, "nuah ART: libart has no JNI_CreateJavaVM\n"); delete jvm; return nullptr; }
  const std::string root = artifact_root();
  const std::string api_jar = root + "/api-impl.jar";
  std::string framework = root + "/framework-res.apk";
  /* Keep framework-res on its verified install path when a copied or symlinked archive would be rejected by the Android zip reader. */
  if (const char* c = env_nonempty("NUAH_ART_FRAMEWORK_RES"); c && std::filesystem::is_regular_file(c)) framework = c;
  else if (const char* c = env_nonempty("NUAH_ATL_FRAMEWORK_RES"); c && std::filesystem::is_regular_file(c)) framework = c;
  if (!std::filesystem::is_regular_file(framework)) { const auto build_framework = std::filesystem::path(root) / "res/framework-res/framework-res.apk"; if (std::filesystem::is_regular_file(build_framework)) framework = build_framework.string(); }
  std::string natives = root + "/natives";
  if (!std::filesystem::is_regular_file(std::filesystem::path(natives) / "libtranslation_layer_main.so") && std::filesystem::is_regular_file(std::filesystem::path(root) / "libtranslation_layer_main.so")) natives = root;
  const auto apk_paths = apk_class_paths();
  if (apk_paths.empty()) { std::fprintf(stderr, "nuah ART: NUAH_APK_PATHS is empty\n"); delete jvm; return nullptr; }
  std::vector<std::string> app_classes{apk_paths.front()};
  jvm->class_path = api_jar + ":" + framework + ":" + join(app_classes);
  const std::string app_path = join(app_classes);
  static std::string app_apk_path;
  app_apk_path = app_classes.front();
  apk_path = app_apk_path.data();
  std::string app_library_path = natives;
  if (const char* app_data = env_nonempty("ANDROID_APP_DATA_DIR")) {
    const std::filesystem::path app_lib = std::filesystem::path(app_data) / "lib";
    const bool explicit_provider = env_nonempty("NUAH_ART_HOME") || env_nonempty("NUAH_ATL_NATIVE_DIR");
    app_library_path = explicit_provider ? natives + ":" + app_lib.string() : app_lib.string() + ":" + natives;
  }
  const std::string library_path = app_library_path;
  std::string class_option = "-Djava.class.path=" + jvm->class_path;
  std::string app_class_option = "-Datl.app.class.path=" + app_path;
  std::string library_option = "-Djava.library.path=" + library_path;
  std::string app_library_option = "-Datl.app.library.path=" + library_path;
  /* The Android provider jars run inside host ART, so their default /system/etc/security/cacerts does not exist. Without a real root store. */
  std::string trust_store_option, trust_store_type_option;
  auto set_trust = [&](const char* p) { trust_store_option = std::string("-Djavax.net.ssl.trustStore=") + p; trust_store_type_option = "-Djavax.net.ssl.trustStoreType=JKS"; };
  if (const char* configured = env_nonempty("NUAH_JAVA_TRUST_STORE"); configured && std::filesystem::is_regular_file(configured)) set_trust(configured);
  else if (!std::getenv("NUAH_DISABLE_JAVA_TRUST_STORE")) {
    static constexpr const char* kTrustStores[] = { "/etc/pki/java/cacerts", "/etc/ssl/certs/java/cacerts", "/etc/ssl/certs/java/cacerts.jks" };
    for (const char* candidate : kTrustStores) {
      if (!std::filesystem::is_regular_file(candidate)) continue;
      set_trust(candidate);
      break;
    }
  }
  /* Roblox's Android HTTP stack opens several short-lived connections during room startup. On a desktop host with an incomplete IPv6 route, the. */
  std::string prefer_ipv4_option = "-Djava.net.preferIPv4Stack=true";
  const bool prefer_ipv4 = !env_nonempty("NUAH_PREFER_IPV4") || env_flag("NUAH_PREFER_IPV4");
  const std::string sdk_option = "-DBuild.VERSION.SDK_INT=36";
  std::string boot_append;
  const std::string dex_root = "/usr/local/lib64/java/dex/art";
  for (const char* jar : {"wolfssljni-hostdex.jar", "bouncycastle-hostdex.jar"}) {
    const std::string path = dex_root + "/" + jar;
    if (std::filesystem::is_regular_file(path)) { if (!boot_append.empty()) boot_append.push_back(':'); boot_append += path; }
  }
  std::string boot_option;
  if (!boot_append.empty()) boot_option = "-Xbootclasspath/a:" + boot_append;
  const std::string bootclasspath = android16_bootclasspath();
  const std::filesystem::path boot_image = "/usr/local/lib64/java/dex/art/oat/boot.art";
  const bool use_boot_image = !env_nonempty("NUAH_ART_USE_BOOT_IMAGE") || env_flag("NUAH_ART_USE_BOOT_IMAGE");
  const bool have_boot_image = use_boot_image && !bootclasspath.empty() && std::filesystem::is_regular_file(boot_image) && std::filesystem::is_regular_file(boot_image.parent_path() / "boot.oat") && std::filesystem::is_regular_file(boot_image.parent_path() / "boot.vdex");
  std::string bootclasspath_option;
  std::string image_option;
  if (have_boot_image) { bootclasspath_option = "-Xbootclasspath:" + bootclasspath; image_option = "-Ximage:" + boot_image.string(); (void)::setenv("BOOTCLASSPATH", bootclasspath.c_str(), 1); art_trace("nuah ART: using boot image %s\n", boot_image.c_str()); }
  else art_trace("nuah ART: boot image unavailable; keeping imageless mode\n");
  std::vector<JavaVMOption> options;
  for (std::string* option : {&library_option, &class_option, &app_class_option, &app_library_option}) options.push_back({option->data(), nullptr});
  if (!trust_store_option.empty()) { options.push_back({trust_store_option.data(), nullptr}); options.push_back({trust_store_type_option.data(), nullptr}); }
  if (prefer_ipv4) { options.push_back({prefer_ipv4_option.data(), nullptr}); }
  /* ART's JNI checker is valuable while validating a new façade, but it wraps every transition with argument/critical-section checks. That is. */
  if (env_flag("NUAH_JNI_CHECK")) options.push_back({"-Xcheck:jni", nullptr});
  options.push_back({const_cast<char*>(sdk_option.c_str()), nullptr});
  if (!bootclasspath_option.empty()) options.push_back({bootclasspath_option.data(), nullptr});
  if (!image_option.empty()) options.push_back({image_option.data(), nullptr});
  else options.push_back({"-Xnoimage-dex2oat", nullptr});
  /* Keep the stable interpreter path for the current ART/Roblox pairing. `-Xusejit:true` currently causes the Android image to unload through a. */
  options.push_back({"-Xusejit:false", nullptr});
  if (!boot_option.empty()) options.push_back({boot_option.data(), nullptr});
  JavaVMInitArgs args{};
  args.version = JNI_VERSION_1_6;
  args.nOptions = static_cast<jint>(options.size());
  args.options = options.data();
  args.ignoreUnrecognized = JNI_FALSE;
  const jint result = create(&jvm->vm, reinterpret_cast<void**>(&jvm->env), &args);
  if (result != JNI_OK || !jvm->vm || !jvm->env) { std::fprintf(stderr, "nuah ART: JNI_CreateJavaVM failed (%d)\n", result); delete jvm; return nullptr; }
  jvm->vm_owner = ::pthread_self();
  jvm->vm_owner_attached = true;
  std::filesystem::path api_native_file = std::filesystem::path(natives) / "libtranslation_layer_main.so";
  if (!std::filesystem::is_regular_file(api_native_file)) {
    if (const char* app_data = env_nonempty("ANDROID_APP_DATA_DIR")) { const auto app_provider = std::filesystem::path(app_data) / "lib" / "libtranslation_layer_main.so"; if (std::filesystem::is_regular_file(app_provider)) api_native_file = app_provider; }
  }
  const std::string api_native_path = api_native_file.string();
  const auto bionic_dlopen = reinterpret_cast<NativeDlopen>(::dlsym(RTLD_DEFAULT, "bionic_dlopen"));
  jvm->api_dlsym = reinterpret_cast<NativeDlsym>(::dlsym(RTLD_DEFAULT, "bionic_dlsym"));
  const bool classloader_loaded = load_atl_library_for_classloader(jvm->env);
  if (classloader_loaded && bionic_dlopen && jvm->api_dlsym) jvm->api_native = bionic_dlopen(api_native_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
  if (!jvm->api_native && bionic_dlopen && jvm->api_dlsym) jvm->api_native = bionic_dlopen(api_native_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
  if (!jvm->api_native) { jvm->api_dlsym = nullptr; jvm->api_native = dlopen(api_native_path.c_str(), RTLD_LAZY | RTLD_GLOBAL); }
  if (!jvm->api_native && trace_enabled()) std::fprintf(stderr, "nuah ART: optional API native load failed: %s\n", dlerror());
  if (jvm->api_native) art_trace("nuah ART: ATL provider loaded once from %s (%s linker, classloader=%s)\n", api_native_path.c_str(), jvm->api_dlsym ? "bionic" : "glibc", classloader_loaded ? "yes" : "no");
  if (!classloader_loaded) art_trace("nuah ART: ATL class-loader registration unavailable; using provider handle only\n");
  static const struct { const char* klass; const char* method; const char* sig; void* fn; } kHostNatives[] = {
      {"android/content/Context", "native_updateConfig", "(Landroid/content/res/Configuration;)V", reinterpret_cast<void*>(&nuah_art_context_update_config)},
      {"android/util/Log", "println_native", "(IILjava/lang/String;Ljava/lang/String;)I", reinterpret_cast<void*>(&nuah_art_log_println)},
      {"com/roblox/universalapp/logging/JNILoggingProtocol", "nativeLogEvent", "(Ljava/lang/String;J[Ljava/lang/Object;)V", reinterpret_cast<void*>(&nuah_art_logging_protocol_native_log_event)},
  };
  for (const auto& entry : kHostNatives) (void)register_host_native(jvm->env, entry.klass, entry.method, entry.sig, entry.fn);
  if (jvm->api_native) {
#define ATL_NAT(cls, pfx, m, sig) {cls, #m, sig, pfx #m}
#define ASSET_NAT(m, sig) ATL_NAT("android/content/res/AssetManager", "Java_android_content_res_AssetManager_", m, sig)
#define XML_NAT(m, sig) ATL_NAT("android/content/res/XmlBlock", "Java_android_content_res_XmlBlock_", m, sig)
#define SYSCLK_NAT(m, sig) ATL_NAT("android/os/SystemClock", "Java_android_os_SystemClock_", m, sig)
#define MSGQ_NAT(m, sig) ATL_NAT("android/os/MessageQueue", "Java_android_os_MessageQueue_", m, sig)
#define SQLITE_NAT(m, sig) ATL_NAT("android/database/sqlite/SQLiteConnection", "Java_android_database_sqlite_SQLiteConnection_", m, sig)
#define ATL_SPEC(cls, m, sig, sym) {cls, m, sig, sym}
#define MATRIX_NAT(m, sig, sym) ATL_SPEC("android/graphics/Matrix", m, sig, "Java_android_graphics_Matrix_" sym)
#define PAINT_NAT(m, sig, sym) ATL_SPEC("android/graphics/Paint", m, sig, "Java_android_graphics_Paint_" sym)
#define VIEW_NAT(m, sig, sym) ATL_SPEC("android/view/View", m, sig, "Java_android_view_View_" sym)
#define WINDOW_NAT(m, sig, sym) ATL_SPEC("android/view/Window", m, sig, "Java_android_view_Window_" sym)
#define DRAWABLE_NAT(m, sig, sym) ATL_SPEC("android/graphics/drawable/Drawable", m, sig, "Java_android_graphics_drawable_Drawable_" sym)
#define VIEWGROUP_NAT(m, sig, sym) ATL_SPEC("android/view/ViewGroup", m, sig, "Java_android_view_ViewGroup_" sym)
#define SURFACE_NAT(m, sig, sym) ATL_SPEC("android/view/SurfaceView", m, sig, "Java_android_view_SurfaceView_" sym)
    static constexpr AtlNativeSpec kAssetAndXmlNatives[] = {
        ASSET_NAT(getPooledString, "(II)Ljava/lang/CharSequence;"), ASSET_NAT(list, "(Ljava/lang/String;)[Ljava/lang/String;"), ASSET_NAT(addAssetPathNative, "(Ljava/lang/String;)I"), ASSET_NAT(isUpToDate, "()Z"),
        ASSET_NAT(setLocale, "(Ljava/lang/String;)V"), ASSET_NAT(getLocales, "()[Ljava/lang/String;"), ASSET_NAT(setConfiguration, "(IILjava/lang/String;IIIIIIIIIIIIII)V"),
        ASSET_NAT(getResourceIdentifier, "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I"), ASSET_NAT(getResourceName, "(I)Ljava/lang/String;"), ASSET_NAT(getResourcePackageName, "(I)Ljava/lang/String;"),
        ASSET_NAT(getResourceTypeName, "(I)Ljava/lang/String;"), ASSET_NAT(getResourceEntryName, "(I)Ljava/lang/String;"), ASSET_NAT(openAsset, "(Ljava/lang/String;I)J"), ASSET_NAT(openAssetFd, "(Ljava/lang/String;I[J[J)I"),
        ASSET_NAT(destroyAsset, "(J)V"), ASSET_NAT(readAssetChar, "(J)I"), ASSET_NAT(readAsset, "(J[BJJ)I"), ASSET_NAT(seekAsset, "(JJI)J"), ASSET_NAT(getAssetLength, "(J)J"), ASSET_NAT(getAssetRemainingLength, "(J)J"),
        ASSET_NAT(loadResourceValue, "(ISLandroid/util/TypedValue;Z)I"), ASSET_NAT(loadResourceBagValue, "(IILandroid/util/TypedValue;Z)I"), ASSET_NAT(applyStyle, "(JJII[IIJJ)V"), ASSET_NAT(resolveAttrs, "(JII[I[I[I[I)Z"),
        ASSET_NAT(retrieveAttributes, "(J[IIJJ)Z"), ASSET_NAT(getArraySize, "(I)I"), ASSET_NAT(retrieveArray, "(I[I)I"), ASSET_NAT(getStringBlockCount, "()I"), ASSET_NAT(getNativeStringBlock, "(I)I"),
        ASSET_NAT(getCookieName, "(I)Ljava/lang/String;"), ASSET_NAT(getGlobalAssetCount, "()I"), ASSET_NAT(getAssetAllocations, "()Ljava/lang/String;"), ASSET_NAT(getGlobalAssetManagerCount, "()I"), ASSET_NAT(newTheme, "()J"),
        ASSET_NAT(deleteTheme, "(J)V"), ASSET_NAT(applyThemeStyle, "(JIZ)V"), ASSET_NAT(copyTheme, "(JJ)V"), ASSET_NAT(loadThemeAttributeValue, "(JILandroid/util/TypedValue;Z)I"),
        ASSET_NAT(dumpTheme, "(JILjava/lang/String;Ljava/lang/String;)V"), ASSET_NAT(openXmlAssetNative, "(ILjava/lang/String;)J"), ASSET_NAT(getArrayStringResource, "(I)[Ljava/lang/String;"), ASSET_NAT(getArrayStringInfo, "(I)[I"),
        ASSET_NAT(init, "(I)V"), ATL_SPEC("android/content/res/AssetManager", "native_setApkAssets", "([Ljava/lang/Object;I)V", "Java_android_content_res_AssetManager_native_1setApkAssets"), XML_NAT(nativeCreate, "([BII)J"),
        XML_NAT(nativeGetStringBlock, "(J)I"), XML_NAT(nativeCreateParseState, "(J)J"), XML_NAT(nativeNext, "(J)I"), XML_NAT(nativeGetNamespace, "(J)I"), XML_NAT(nativeGetName, "(J)Ljava/lang/String;"), XML_NAT(nativeGetText, "(J)I"),
        XML_NAT(nativeGetLineNumber, "(J)I"), XML_NAT(nativeGetAttributeCount, "(J)I"), XML_NAT(nativeGetAttributeNamespace, "(JI)I"), XML_NAT(nativeGetAttributeName, "(JI)I"), XML_NAT(nativeGetAttributeResource, "(JI)I"),
        XML_NAT(nativeGetAttributeDataType, "(JI)I"), XML_NAT(nativeGetAttributeData, "(JI)I"), XML_NAT(nativeGetAttributeStringValue, "(JI)Ljava/lang/String;"), XML_NAT(nativeGetIdAttribute, "(J)I"),
        XML_NAT(nativeGetClassAttribute, "(J)Ljava/lang/String;"), XML_NAT(nativeGetStyleAttribute, "(J)I"), XML_NAT(nativeGetAttributeIndex, "(JLjava/lang/String;Ljava/lang/String;)I"), XML_NAT(nativeDestroyParseState, "(J)V"),
        XML_NAT(nativeGetPooledString, "(JI)Ljava/lang/String;"), XML_NAT(nativeDestroy, "(J)V"),
        ATL_SPEC("android/os/Environment", "native_get_app_data_dir", "()Ljava/lang/String;", "Java_android_os_Environment_native_1get_1app_1data_1dir"),
        ATL_SPEC("android/content/Context", "native_get_apk_path", "()Ljava/lang/String;", "Java_android_content_Context_native_1get_1apk_1path"), SYSCLK_NAT(uptimeMillis, "()J"), SYSCLK_NAT(elapsedRealtime, "()J"),
        SYSCLK_NAT(elapsedRealtimeNanos, "()J"), SYSCLK_NAT(currentThreadTimeMillis, "()J"), MSGQ_NAT(nativeInit, "()J"), MSGQ_NAT(nativeDestroy, "(J)V"), MSGQ_NAT(nativePollOnce, "(JI)Z"), MSGQ_NAT(nativeWake, "(J)V"),
        MSGQ_NAT(nativeIsIdling, "(J)Z"), ATL_SPEC("android/database/sqlite/SQLiteGlobal", "nativeReleaseMemory", "()I", "Java_android_database_sqlite_SQLiteGlobal_nativeReleaseMemory"),
        SQLITE_NAT(nativeOpen, "(Ljava/lang/String;ILjava/lang/String;ZZ)J"), SQLITE_NAT(nativeClose, "(J)V"), SQLITE_NAT(nativeRegisterCustomFunction, "(JLandroid/database/sqlite/SQLiteCustomFunction;)V"),
        SQLITE_NAT(nativeRegisterLocalizedCollators, "(JLjava/lang/String;)V"), SQLITE_NAT(nativePrepareStatement, "(JLjava/lang/String;)J"), SQLITE_NAT(nativeFinalizeStatement, "(JJ)V"), SQLITE_NAT(nativeGetParameterCount, "(JJ)I"),
        SQLITE_NAT(nativeIsReadOnly, "(JJ)Z"), SQLITE_NAT(nativeGetColumnCount, "(JJ)I"), SQLITE_NAT(nativeGetColumnName, "(JJI)Ljava/lang/String;"), SQLITE_NAT(nativeBindNull, "(JJI)V"), SQLITE_NAT(nativeBindLong, "(JJIJ)V"),
        SQLITE_NAT(nativeBindDouble, "(JJID)V"), SQLITE_NAT(nativeBindString, "(JJILjava/lang/String;)V"), SQLITE_NAT(nativeBindBlob, "(JJI[B)V"), SQLITE_NAT(nativeResetStatementAndClearBindings, "(JJ)V"),
        SQLITE_NAT(nativeExecute, "(JJ)V"), SQLITE_NAT(nativeExecuteForLong, "(JJ)J"), SQLITE_NAT(nativeExecuteForString, "(JJ)Ljava/lang/String;"), SQLITE_NAT(nativeExecuteForChangedRowCount, "(JJ)I"),
        SQLITE_NAT(nativeExecuteForLastInsertedRowId, "(JJ)J"), SQLITE_NAT(nativeExecuteForCursorWindow, "(JJLandroid/database/CursorWindow;IIZ)J"), SQLITE_NAT(nativeGetDbLookaside, "(J)I"), SQLITE_NAT(nativeCancel, "(J)V"),
        SQLITE_NAT(nativeResetCancel, "(JZ)V"), SQLITE_NAT(nativeHasCodec, "()Z"),
        ATL_SPEC("android/net/ConnectivityManager", "registerNetworkCallback", "(Landroid/net/NetworkRequest;Landroid/net/ConnectivityManager$NetworkCallback;)V", "Java_android_net_ConnectivityManager_registerNetworkCallback"),
        ATL_SPEC("android/net/ConnectivityManager", "isActiveNetworkMetered", "()Z", "Java_android_net_ConnectivityManager_isActiveNetworkMetered"),
        ATL_SPEC("android/net/ConnectivityManager", "nativeGetNetworkAvailable", "()Z", "Java_android_net_ConnectivityManager_nativeGetNetworkAvailable"),
        SURFACE_NAT("native_constructor", "(Landroid/content/Context;Landroid/util/AttributeSet;)J", "native_1constructor"), SURFACE_NAT("nativeDirectAppStart", "()V", "nativeDirectAppStart"),
        SURFACE_NAT("native_createSnapshot", "()J", "native_1createSnapshot"), SURFACE_NAT("native_postSnapshot", "(JJ)V", "native_1postSnapshot"),
        ATL_SPEC("android/view/inputmethod/InputMethodManager", "nativeInit", "()J", "Java_android_view_inputmethod_InputMethodManager_nativeInit"),
        ATL_SPEC("android/view/inputmethod/InputMethodManager", "nativeShowSoftInput", "(JJLandroid/view/inputmethod/InputConnection;I)Z", "Java_android_view_inputmethod_InputMethodManager_nativeShowSoftInput"),
        ATL_SPEC("android/view/inputmethod/InputMethodManager", "nativeHideSoftInput", "(J)V", "Java_android_view_inputmethod_InputMethodManager_nativeHideSoftInput"),
        VIEWGROUP_NAT("native_addView", "(JJILandroid/view/ViewGroup$LayoutParams;)V", "native_1addView"), VIEWGROUP_NAT("native_removeView", "(JJ)V", "native_1removeView"),
        VIEWGROUP_NAT("native_drawChildren", "(JJ)V", "native_1drawChildren"), VIEWGROUP_NAT("native_drawChild", "(JJJ)V", "native_1drawChild"),
        VIEWGROUP_NAT("native_dispatchTouchEvent", "(JLandroid/view/MotionEvent;DD)Z", "native_1dispatchTouchEvent"), DRAWABLE_NAT("native_paintable_from_path", "(Ljava/lang/String;)J", "native_1paintable_1from_1path"),
        DRAWABLE_NAT("native_constructor", "()J", "native_1constructor"), DRAWABLE_NAT("native_invalidate", "(J)V", "native_1invalidate"), DRAWABLE_NAT("native_draw", "(JJII)V", "native_1draw"),
        DRAWABLE_NAT("native_ref", "(J)V", "native_1ref"), DRAWABLE_NAT("native_unref", "(J)V", "native_1unref"), MATRIX_NAT("native_create", "(J)J", "native_1create"), MATRIX_NAT("native_getValues", "(J[F)V", "native_1getValues"),
        MATRIX_NAT("native_set", "(JJ)V", "native_1set"), MATRIX_NAT("native_isIdentity", "(J)Z", "native_1isIdentity"), MATRIX_NAT("native_rectStaysRect", "(J)Z", "native_1rectStaysRect"),
        MATRIX_NAT("native_reset", "(J)V", "native_1reset"), MATRIX_NAT("native_setTranslate", "(JFF)V", "native_1setTranslate"), MATRIX_NAT("native_setScale", "(JFF)V", "native_1setScale__JFF"),
        MATRIX_NAT("native_setScale", "(JFFFF)V", "native_1setScale__JFFFF"), MATRIX_NAT("native_setRotate", "(JF)V", "native_1setRotate__JF"), MATRIX_NAT("native_setRotate", "(JFFF)V", "native_1setRotate__JFFF"),
        MATRIX_NAT("native_preTranslate", "(JFF)Z", "native_1preTranslate"), MATRIX_NAT("native_preScale", "(JFF)Z", "native_1preScale__JFF"), MATRIX_NAT("native_preScale", "(JFFFF)Z", "native_1preScale__JFFFF"),
        MATRIX_NAT("native_preRotate", "(JF)Z", "native_1preRotate__JF"), MATRIX_NAT("native_preRotate", "(JFFF)Z", "native_1preRotate__JFFF"), MATRIX_NAT("native_preConcat", "(JJ)Z", "native_1preConcat"),
        MATRIX_NAT("native_postTranslate", "(JFF)Z", "native_1postTranslate"), MATRIX_NAT("native_postScale", "(JFF)Z", "native_1postScale__JFF"), MATRIX_NAT("native_postScale", "(JFFFF)Z", "native_1postScale__JFFFF"),
        MATRIX_NAT("native_postRotate", "(JF)Z", "native_1postRotate__JF"), MATRIX_NAT("native_postRotate", "(JFFF)Z", "native_1postRotate__JFFF"), MATRIX_NAT("native_postConcat", "(JJ)Z", "native_1postConcat"),
        MATRIX_NAT("native_setRectToRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;I)Z", "native_1setRectToRect"), MATRIX_NAT("native_invert", "(JJ)Z", "native_1invert"),
        MATRIX_NAT("native_mapPoints", "(J[FI[FIIZ)V", "native_1mapPoints"), MATRIX_NAT("native_mapRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;)Z", "native_1mapRect"),
        MATRIX_NAT("native_setValues", "(J[F)V", "native_1setValues"), MATRIX_NAT("native_equals", "(JJ)Z", "native_1equals"), MATRIX_NAT("finalizer", "(J)V", "finalizer"), PAINT_NAT("native_create", "()J", "native_1create"),
        PAINT_NAT("native_clone", "(J)J", "native_1clone"), PAINT_NAT("native_recycle", "(J)V", "native_1recycle"), PAINT_NAT("native_set_color", "(JI)V", "native_1set_1color"), PAINT_NAT("native_get_color", "(J)I", "native_1get_1color"),
        PAINT_NAT("native_set_alpha", "(JI)V", "native_1set_1alpha"), PAINT_NAT("native_get_alpha", "(J)I", "native_1get_1alpha"), PAINT_NAT("native_set_style", "(JI)V", "native_1set_1style"),
        PAINT_NAT("native_get_style", "(J)I", "native_1get_1style"), PAINT_NAT("native_set_stroke_width", "(JF)V", "native_1set_1stroke_1width"), PAINT_NAT("native_get_stroke_width", "(J)F", "native_1get_1stroke_1width"),
        PAINT_NAT("native_set_stroke_cap", "(JI)V", "native_1set_1stroke_1cap"), PAINT_NAT("native_get_stroke_cap", "(J)I", "native_1get_1stroke_1cap"), PAINT_NAT("native_set_stroke_join", "(JI)V", "native_1set_1stroke_1join"),
        PAINT_NAT("native_get_stroke_join", "(J)I", "native_1get_1stroke_1join"), PAINT_NAT("native_set_text_size", "(JF)V", "native_1set_1text_1size"), PAINT_NAT("native_get_text_size", "(J)F", "native_1get_1text_1size"),
        PAINT_NAT("native_set_color_filter", "(JII)V", "native_1set_1color_1filter"), PAINT_NAT("native_get_text_bounds", "(JLjava/lang/String;Landroid/graphics/Rect;)V", "native_1get_1text_1bounds"),
        PAINT_NAT("native_set_text_align", "(JI)V", "native_1set_1text_1align"), VIEW_NAT("native_constructor", "(Landroid/content/Context;Landroid/util/AttributeSet;)J", "native_1constructor"),
        VIEW_NAT("nativeSetOnTouchListener", "(J)V", "nativeSetOnTouchListener"), VIEW_NAT("nativeSetOnClickListener", "(J)V", "nativeSetOnClickListener"), VIEW_NAT("native_setLayoutParams", "(JIIIFIIII)V", "native_1setLayoutParams"),
        VIEW_NAT("native_destructor", "(J)V", "native_1destructor"), VIEW_NAT("native_measure", "(JII)V", "native_1measure"), VIEW_NAT("native_layout", "(JIIII)V", "native_1layout"),
        VIEW_NAT("native_requestLayout", "(J)V", "native_1requestLayout"), VIEW_NAT("native_setBackgroundDrawable", "(JJ)V", "native_1setBackgroundDrawable"), VIEW_NAT("native_queueAllocate", "(J)V", "native_1queueAllocate"),
        VIEW_NAT("native_addClass", "(JLjava/lang/String;)V", "native_1addClass"), VIEW_NAT("native_removeClass", "(JLjava/lang/String;)V", "native_1removeClass"),
        VIEW_NAT("native_addClasses", "(J[Ljava/lang/String;)V", "native_1addClasses"), VIEW_NAT("native_removeClasses", "(J[Ljava/lang/String;)V", "native_1removeClasses"),
        VIEW_NAT("native_drawBackground", "(JJ)V", "native_1drawBackground"), VIEW_NAT("native_drawContent", "(JJ)V", "native_1drawContent"), VIEW_NAT("nativeRequestFocus", "(JI)V", "nativeRequestFocus"),
        VIEW_NAT("nativeSetFullscreen", "(JZ)V", "nativeSetFullscreen"), VIEW_NAT("native_get_window", "(J)Landroid/view/Window;", "native_1get_1window"), VIEW_NAT("nativeInvalidate", "(J)V", "nativeInvalidate"),
        VIEW_NAT("native_setBackgroundColor", "(JI)V", "native_1setBackgroundColor"), VIEW_NAT("native_setVisibility", "(JIFZ)V", "native_1setVisibility"), VIEW_NAT("native_setPadding", "(JIIII)V", "native_1setPadding"),
        VIEW_NAT("nativeSetOnLongClickListener", "(J)V", "nativeSetOnLongClickListener"), VIEW_NAT("nativeIsFocused", "(J)Z", "nativeIsFocused"), VIEW_NAT("native_getMatrix", "(JJ)Z", "native_1getMatrix"),
        VIEW_NAT("nativeIsAttachedToWindow", "(J)Z", "nativeIsAttachedToWindow"), VIEW_NAT("native_keep_screen_on", "(JZ)V", "native_1keep_1screen_1on"),
        VIEW_NAT("native_getGlobalVisibleRect", "(JLandroid/graphics/Rect;)Z", "native_1getGlobalVisibleRect"), VIEW_NAT("getWindowVisibleDisplayFrame", "(Landroid/graphics/Rect;)V", "getWindowVisibleDisplayFrame"),
        WINDOW_NAT("set_jobject", "(JLandroid/view/Window;)V", "set_1jobject"), WINDOW_NAT("set_widget_as_root", "(JJ)V", "set_1widget_1as_1root"), WINDOW_NAT("set_title", "(JLjava/lang/String;)V", "set_1title"),
        WINDOW_NAT("take_input_queue", "(JLandroid/view/InputQueue$Callback;Landroid/view/InputQueue;)V", "take_1input_1queue"), WINDOW_NAT("set_layout", "(JII)V", "set_1layout"),
        WINDOW_NAT("remove_gtk_background", "(J)V", "remove_1gtk_1background"), WINDOW_NAT("set_screen_brightness", "(F)V", "set_1screen_1brightness"),
    };
#undef ATL_NAT
#undef ASSET_NAT
#undef XML_NAT
#undef SYSCLK_NAT
#undef MSGQ_NAT
#undef SQLITE_NAT
#undef ATL_SPEC
#undef MATRIX_NAT
#undef PAINT_NAT
#undef VIEW_NAT
#undef WINDOW_NAT
#undef DRAWABLE_NAT
#undef VIEWGROUP_NAT
#undef SURFACE_NAT
    register_atl_natives(jvm->env, jvm->api_native, jvm->api_dlsym, kAssetAndXmlNatives, sizeof(kAssetAndXmlNatives) / sizeof(kAssetAndXmlNatives[0]));
  }
  initialize_atl_host_state(jvm);
  force_android_sdk_level(jvm->env, 36);
  if (!initialize_roblox_device_static_params(jvm)) art_trace("nuah ART: Roblox DeviceStaticParams setup was unavailable\n");
  jclass activity_class = find_class(jvm->env, "com/google/androidgamesdk/GameActivity");
  if (!activity_class) { std::fprintf(stderr, "nuah ART: GameActivity is not on the APK class path\n"); delete jvm; return nullptr; }
  jclass looper_class = find_class(jvm->env, "android/os/Looper");
  if (looper_class) {
    const jmethodID prepare = jvm->env->GetStaticMethodID(looper_class, "prepareMainLooper", "()V");
    if (prepare) jvm->env->CallStaticVoidMethod(looper_class, prepare);
    clear_exception(jvm->env, "Looper.prepareMainLooper");
  }
  jobject activity = nullptr;
  jclass activity_helper = find_class(jvm->env, "android/app/Activity");
  jclass intent_class = find_class(jvm->env, "android/content/Intent");
  const jmethodID intent_ctor = intent_class ? jvm->env->GetMethodID(intent_class, "<init>", "()V") : nullptr;
  jobject intent = intent_ctor ? jvm->env->NewObject(intent_class, intent_ctor) : nullptr;
  const jmethodID create_activity =
      activity_helper ? jvm->env->GetStaticMethodID(activity_helper, "internalCreateActivity", "(Ljava/lang/String;JLandroid/content/Intent;)Landroid/app/Activity;") : nullptr;
  jstring main_activity_name = jvm->env->NewStringUTF("com.roblox.client.startup.MainGameActivity");
  if (create_activity && intent && main_activity_name) { activity = jvm->env->CallStaticObjectMethod(activity_helper, create_activity, main_activity_name, static_cast<jlong>(0), intent); }
  if (!activity || jvm->env->ExceptionCheck()) { clear_exception(jvm->env, "ATL Activity.internalCreateActivity"); activity = nullptr; }
  if (!activity) { std::fprintf(stderr, "nuah ART: ATL could not construct MainGameActivity with an attached Context\n"); delete jvm; return nullptr; }
  jvm->activity = jvm->env->NewGlobalRef(activity);
  clear_exception(jvm->env, "GameActivity global reference");
  if (!jvm->activity) { delete jvm; return nullptr; }
  art_trace("nuah ART: VM ready (%s) activity=%p\n", art_path().c_str(), static_cast<void*>(jvm->activity));
  return jvm;
}
extern "C" void nuah_jvm_destroy(NuahJvm* jvm) {
  if (!jvm) return;
  nuah_jvm_clear_surface(jvm);
  delete_global(jvm->env, jvm->activity); delete_global(jvm->env, jvm->key_event); delete_global(jvm->env, jvm->motion_event);
  delete_global_class(jvm->env, jvm->key_event_class); delete_global_class(jvm->env, jvm->motion_event_class);
  delete_global_class(jvm->env, jvm->native_input_interface); delete_global_class(jvm->env, jvm->native_gl_interface);
  /* Detach only the thread that called JNI_CreateJavaVM. The VM remains resident; this merely closes ART's TLS/thread-state record before the. */
  if (jvm->vm && jvm->vm_owner_attached && ::pthread_equal(jvm->vm_owner, ::pthread_self())) { (void)jvm->vm->DetachCurrentThread(); jvm->vm_owner_attached = false; }
  delete jvm;
}
extern "C" void* nuah_jvm_java_vm(NuahJvm* jvm) { return jvm ? static_cast<void*>(jvm->vm) : nullptr; }
extern "C" void* nuah_jvm_jni_env(NuahJvm* jvm) { return jvm ? static_cast<void*>(jvm->env) : nullptr; }
extern "C" void* nuah_jvm_find_registered_native(NuahJvm* jvm, const char* class_name, const char* method_name, const char* signature) {
  if (!jvm || !class_name || !method_name || !signature) return nullptr;
  jmethodID method = find_instance_method(jvm->env, class_name, method_name, signature);
  return method ? reinterpret_cast<void*>(method) : nullptr;
}
extern "C" int nuah_jvm_bind_native(NuahJvm* jvm, const char* class_name, const char* method_name, const char* signature, void* function) {
  if (!jvm || !function) return 0;
  jclass klass = find_class(jvm->env, class_name);
  if (!klass) return 0;
  const JNINativeMethod method{method_name, signature, function};
  const jint result = jvm->env->RegisterNatives(klass, &method, 1);
  art_trace("nuah ART: RegisterNatives %s.%s %s fn=%p result=%d\n", class_name, method_name, signature, function, static_cast<int>(result));
  if (result != JNI_OK) clear_exception(jvm->env, "RegisterNatives");
  return result == JNI_OK;
}
extern "C" void* nuah_jvm_game_activity(NuahJvm* jvm) { return jvm ? jvm->activity : nullptr; }
extern "C" int nuah_jvm_dispatch_application_create(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return 0;
  art_trace("nuah ART: app-state Context.createApplication begin\n");
  jclass context_class = find_class(jvm->env, "android/content/Context");
  if (!context_class) return 0;
  const jmethodID create_application = jvm->env->GetStaticMethodID(context_class, "createApplication", "(J)Landroid/app/Application;");
  if (!create_application) { clear_exception(jvm->env, "Context.createApplication lookup"); return 0; }
  jobject application = jvm->env->CallStaticObjectMethod(context_class, create_application, static_cast<jlong>(0));
  if (!application || check_clear(jvm->env, "Context.createApplication")) return 0;
  art_trace("nuah ART: app-state Context.createApplication done\n");
  if (!std::getenv("NUAH_SKIP_WORKMANAGER_INIT")) {
    jclass work_initializer = find_class(jvm->env, "androidx/work/WorkManagerInitializer");
    if (work_initializer) {
      const jmethodID initializer_ctor = jvm->env->GetMethodID(work_initializer, "<init>", "()V"); const jmethodID initializer_create = jvm->env->GetMethodID(work_initializer, "create", "(Landroid/content/Context;)Ljava/lang/Object;");
      if (initializer_ctor && initializer_create) {
        jobject initializer = jvm->env->NewObject(work_initializer, initializer_ctor);
        if (initializer) { (void)jvm->env->CallObjectMethod(initializer, initializer_create, application); if (jvm->env->ExceptionCheck()) clear_exception(jvm->env, "WorkManagerInitializer.create"); }
      } else clear_exception(jvm->env, "WorkManagerInitializer methods");
    }
  } else art_trace("nuah ART: skipping WorkManager pre-initialization\n");
  jclass roblox_application = find_class(jvm->env, "com/roblox/client/RobloxApplication");
  if (!roblox_application) return 0;
  art_trace("nuah ART: app-state RobloxApplication class ready\n");
  const char* application_field_name = "b";
  jfieldID application_field = jvm->env->GetStaticFieldID(roblox_application, application_field_name, "Landroid/content/Context;");
  /* Roblox 2.734 moved the static Application context from b to c while reusing b for a long-lived native/session value. Keep the old contract. */
  if (!application_field) { clear_exception(jvm->env, "RobloxApplication.b lookup"); application_field_name = "c";
    application_field = jvm->env->GetStaticFieldID(roblox_application, application_field_name, "Landroid/content/Context;"); if (application_field) art_trace("nuah ART: app-state context moved to RobloxApplication.c\n"); }
  if (!application_field) { clear_exception(jvm->env, "RobloxApplication context lookup"); return 0; }
  jvm->env->SetStaticObjectField(roblox_application, application_field, application);
  if (check_clear(jvm->env, "RobloxApplication context")) return 0;
  art_trace("nuah ART: app-state RobloxApplication.%s set\n", application_field_name);
  jclass paths = find_class(jvm->env, "rh/y0");
  if (!paths) { clear_exception(jvm->env, "rh.y0 class lookup"); art_trace("nuah ART: optional rh.y0 bootstrap absent\n"); return 1; }
  art_trace("nuah ART: app-state %s class ready\n", "rh.y0");
  jmethodID prepare_paths = jvm->env->GetStaticMethodID(paths, "e0", "(Landroid/content/Context;)V");
  /* The current APK moved this exact helper from rh.y0 to rh.z0. Select it by contract (method signature), not by an obfuscated class name. */
  if (!prepare_paths) {
    clear_exception(jvm->env, "rh.y0.e0 optional lookup");
    jclass moved_paths = find_class(jvm->env, "rh/z0");
    if (moved_paths) {
      const jmethodID moved_prepare = jvm->env->GetStaticMethodID(moved_paths, "e0", "(Landroid/content/Context;)V");
      if (moved_prepare) { paths = moved_paths; prepare_paths = moved_prepare; art_trace("nuah ART: app-state helper moved to rh.z0\n"); }
      else clear_exception(jvm->env, "rh.z0.e0 optional lookup");
    } else clear_exception(jvm->env, "rh.z0 class lookup");
  }
  if (prepare_paths) { jvm->env->CallStaticVoidMethod(paths, prepare_paths, application); if (!check_clear(jvm->env, "rh.y0.e0")) art_trace("nuah ART: app-state rh.y0.e0 done\n"); }
  else { clear_exception(jvm->env, "rh.y0.e0 optional lookup"); art_trace("nuah ART: optional rh.y0.e0 absent; continuing\n"); }
  const jmethodID prefs_factory = jvm->env->GetStaticMethodID(paths, "S", "(Landroid/content/Context;)Landroid/content/SharedPreferences;");
  if (prefs_factory) {
    jobject prefs = jvm->env->CallStaticObjectMethod(paths, prefs_factory, application);
    if (prefs && !check_clear(jvm->env, "rh.y0.S optional")) {
      art_trace("nuah ART: app-state rh.y0.S done\n");
      const jfieldID prefs_field = jvm->env->GetStaticFieldID(paths, "r", "Landroid/content/SharedPreferences;");
      if (prefs_field) {
        jvm->env->SetStaticObjectField(paths, prefs_field, prefs);
        if (!check_clear(jvm->env, "rh.y0.r")) art_trace("nuah ART: app-state rh.y0.r set\n");
      } else clear_exception(jvm->env, "rh.y0.r optional lookup");
    }
  } else {
    clear_exception(jvm->env, "rh.y0.S optional lookup");
    art_trace("nuah ART: optional rh.y0.S/r absent; continuing\n");
    /* Roblox 2.734 moved the shared-preferences bootstrap to rh.w0. Its singleton reads rh.w0.r during class initialization, so seed that field. */
    jclass moved_preferences = find_class(jvm->env, "rh/w0");
    if (moved_preferences) {
      const jmethodID preferences_provider = jvm->env->GetStaticMethodID(moved_preferences, "T", "(Landroid/content/Context;)Landroid/content/SharedPreferences;");
      if (preferences_provider) {
        jobject preferences = jvm->env->CallStaticObjectMethod(moved_preferences, preferences_provider, application);
        if (preferences && !check_clear(jvm->env, "rh.w0.T")) {
          const jfieldID moved_preferences_field = jvm->env->GetStaticFieldID(moved_preferences, "r", "Landroid/content/SharedPreferences;");
          if (moved_preferences_field) {
            jvm->env->SetStaticObjectField(moved_preferences, moved_preferences_field, preferences);
            if (!check_clear(jvm->env, "rh.w0.r")) art_trace("nuah ART: app-state rh.w0.T/r initialized\n");
          } else clear_exception(jvm->env, "rh.w0.r lookup");
        }
      } else clear_exception(jvm->env, "rh.w0.T lookup");
    } else clear_exception(jvm->env, "rh.w0 class lookup");
  }
  return 1;
}
extern "C" int nuah_jvm_dispatch_activity_create(NuahJvm* jvm) {
  if (!jvm || !jvm->env || !jvm->activity) return 0;
  jclass activity_class = find_class(jvm->env, "com/roblox/client/startup/MainGameActivity");
  jclass bundle_class = find_class(jvm->env, "android/os/Bundle");
  if (!activity_class || !bundle_class) return 0;
  const jmethodID bundle_ctor = jvm->env->GetMethodID(bundle_class, "<init>", "()V");
  const jmethodID on_create = jvm->env->GetMethodID(activity_class, "onCreate", "(Landroid/os/Bundle;)V");
  if (!bundle_ctor || !on_create) { clear_exception(jvm->env, "MainGameActivity.onCreate lookup"); return 0; }
  jobject bundle = jvm->env->NewObject(bundle_class, bundle_ctor);
  if (!bundle) { clear_exception(jvm->env, "MainGameActivity.onCreate Bundle"); return 0; }
  jvm->env->CallVoidMethod(jvm->activity, on_create, bundle);
  if (check_clear(jvm->env, "MainGameActivity.onCreate")) return 0;
  return 1;
}
extern "C" int nuah_jvm_capture_native_handle(NuahJvm* jvm) {
  if (!jvm || !jvm->env) return 0;
  jclass activity_class = find_class(jvm->env, "com/google/androidgamesdk/GameActivity");
  if (!activity_class) return 0;
  const jfieldID handle = jvm->env->GetStaticFieldID(activity_class, "P", "J");
  if (!handle) { clear_exception(jvm->env, "GameActivity native handle lookup"); return 0; }
  const jlong value = jvm->env->GetStaticLongField(activity_class, handle);
  if (jvm->env->ExceptionCheck() || value == 0) { clear_exception(jvm->env, "GameActivity native handle read"); return 0; }
  jvm->native_handle = value;
  return 1;
}
extern "C" void* nuah_jvm_key_event(NuahJvm* jvm, int keycode, int action, int repeat, int scancode, unsigned int modifiers, unsigned long long event_time_ms) {
  if (!jvm || !jvm->env) return nullptr;
  if (!jvm->key_event_lookup_done) {
    jvm->key_event_lookup_done = true;
    if (ensure_global_class(jvm->env, jvm->key_event_class, "android/view/KeyEvent")) {
      jvm->key_event_ctor = jvm->env->GetMethodID(jvm->key_event_class, "<init>", "(JJIIIIIIII)V");
      if (jvm->key_event_ctor) jvm->key_event_extended = true;
      else {
        clear_exception(jvm->env, "KeyEvent constructor");
        jvm->key_event_ctor = jvm->env->GetMethodID(jvm->key_event_class, "<init>", "(JJIIII)V");
        if (!jvm->key_event_ctor) clear_exception(jvm->env, "KeyEvent fallback constructor");
      }
    }
  }
  if (!jvm->key_event_class || !jvm->key_event_ctor) return nullptr;
  jobject local = nullptr;
  if (jvm->key_event_extended) {
    local = jvm->env->NewObject(jvm->key_event_class, jvm->key_event_ctor, static_cast<jlong>(event_time_ms), static_cast<jlong>(event_time_ms), action, keycode, repeat, static_cast<jint>(modifiers),
                                0, scancode, 0, 0x101);
  }
  if (!local) { clear_exception(jvm->env, "KeyEvent allocation"); local = jvm->env->NewObject(jvm->key_event_class, jvm->key_event_ctor, static_cast<jlong>(event_time_ms), static_cast<jlong>(event_time_ms), action, keycode, repeat, static_cast<jint>(modifiers)); }
  return new_global_event(jvm->env, jvm->key_event, local);
}
extern "C" void* nuah_jvm_motion_event(NuahJvm* jvm, int action, int /*button*/, double x, double y, double dx, double dy, unsigned long long event_time_ms) {
  if (!jvm || !jvm->env) return nullptr;
  if (!jvm->motion_event_lookup_done) {
    jvm->motion_event_lookup_done = true;
    if (ensure_global_class(jvm->env, jvm->motion_event_class, "android/view/MotionEvent")) {
      jvm->motion_event_ctor = jvm->env->GetMethodID(jvm->motion_event_class, "<init>", "(IIJFFFF)V");
      if (!jvm->motion_event_ctor) clear_exception(jvm->env, "MotionEvent constructor");
    }
  }
  if (!jvm->motion_event_class || !jvm->motion_event_ctor) return nullptr;
  jobject local = jvm->env->NewObject(jvm->motion_event_class, jvm->motion_event_ctor, 0x1002, action, static_cast<jlong>(event_time_ms), static_cast<jfloat>(x), static_cast<jfloat>(y),
                                      static_cast<jfloat>(x + dx), static_cast<jfloat>(y + dy));
  if (!local) { clear_exception(jvm->env, "MotionEvent allocation"); return nullptr; }
  return new_global_event(jvm->env, jvm->motion_event, local);
}
extern "C" void* nuah_jvm_surface(NuahJvm* jvm, NuahNativeWindow* window) {
  if (!jvm || !window) return nullptr;
  /* Once MainGameActivity.onCreate has run, GameActivity owns the actual SurfaceView/SurfaceHolder pair. Use that object for Roblox calls. */
  jobject activity_surface = nullptr;
  if (jvm->activity) {
    jclass game_activity = find_class(jvm->env, "com/google/androidgamesdk/GameActivity");
    if (game_activity) {
      const jfieldID holder_field = jvm->env->GetFieldID(game_activity, "I", "Landroid/view/SurfaceHolder;");
      jobject holder = holder_field ? jvm->env->GetObjectField(jvm->activity, holder_field) : nullptr;
      /* GameActivity assigns I from surfaceCreated(), which may not have run yet when Nuah is about to start the URI. H is created by. */
      if (!holder) {
        clear_exception(jvm->env, "GameActivity holder field");
        const jfieldID view_field = jvm->env->GetFieldID(game_activity, "H", "Lcom/google/androidgamesdk/GameActivity$d;");
        jobject view = view_field ? jvm->env->GetObjectField(jvm->activity, view_field) : nullptr;
        if (view) {
          jclass view_class = find_class(jvm->env, "android/view/SurfaceView");
          const jmethodID get_holder = view_class ? jvm->env->GetMethodID(view_class, "getHolder", "()Landroid/view/SurfaceHolder;") : nullptr;
          if (get_holder) holder = jvm->env->CallObjectMethod(view, get_holder);
          clear_exception(jvm->env, "GameActivity SurfaceView.getHolder");
        }
      }
      if (holder) {
        jclass holder_class = find_class(jvm->env, "android/view/SurfaceHolder");
        const jmethodID get_surface = holder_class ? jvm->env->GetMethodID(holder_class, "getSurface", "()Landroid/view/Surface;") : nullptr;
        if (get_surface) activity_surface = jvm->env->CallObjectMethod(holder, get_surface);
        clear_exception(jvm->env, "GameActivity SurfaceHolder.getSurface");
      }
      clear_exception(jvm->env, "GameActivity SurfaceHolder lookup");
    }
  }
  if (!activity_surface && jvm->surface && jvm->surface_window == window) return jvm->surface;
  if (jvm->surface) { nuah_native_window_unregister_surface(jvm->surface); delete_global(jvm->env, jvm->surface); jvm->surface = nullptr; jvm->surface_window = nullptr; }
  if (activity_surface) {
    jobject stored = store_surface(jvm, window, jvm->env->NewGlobalRef(activity_surface));
    /* Replacing the pre-create façade unregisters its alias, which clears the default entry as a safety measure. Keep the same host window as the. */
    if (!stored) return nullptr;
    art_trace("nuah ART: using GameActivity holder Surface %p\n", static_cast<void*>(stored));
    return stored;
  }
  jclass klass = find_class(jvm->env, "android/view/Surface");
  if (!klass) return nullptr;
  jobject local = jvm->env->AllocObject(klass);
  if (!local) { clear_exception(jvm->env, "Surface allocation"); return nullptr; }
  jfieldID widget = jvm->env->GetFieldID(klass, "widget", "J");
  if (widget) jvm->env->SetLongField(local, widget, reinterpret_cast<jlong>(window));
  clear_exception(jvm->env, "Surface widget");
  return store_surface(jvm, window, jvm->env->NewGlobalRef(local));
}
extern "C" void nuah_jvm_clear_surface(NuahJvm* jvm) {
  if (!jvm || !jvm->surface) return;
  nuah_native_window_unregister_surface(jvm->surface);
  delete_global(jvm->env, jvm->surface);
  jvm->surface = nullptr;
  jvm->surface_window = nullptr;
}
extern "C" long long nuah_jvm_initialize_game(NuahJvm* jvm, const char* /*package_name*/, const char* data_path) {
  if (!jvm || !jvm->activity) return 0;
  jmethodID method = NUAH_GAME_METHOD(jvm->env, "initializeNativeCode",
                                      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                                      "Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J");
  if (!method) return 0;
  jobject assets = make_empty(jvm->env, "android/content/res/AssetManager");
  jobject configuration = make_empty(jvm->env, "android/content/res/Configuration");
  jbyteArray saved = jvm->env->NewByteArray(0);
  const std::string root = data_path ? data_path : "";
  const std::string obb = root + "/obb";
  const std::string external = root + "/external";
  const jlong result = jvm->env->CallLongMethod(jvm->activity, method, jvm->env->NewStringUTF(root.c_str()), jvm->env->NewStringUTF(obb.c_str()), jvm->env->NewStringUTF(external.c_str()), assets,
                                                saved, configuration);
  if (check_clear(jvm->env, "GameActivity.initializeNativeCode")) return 0;
  jvm->native_handle = result;
  return result;
}
extern "C" int nuah_jvm_dispatch_lifecycle(NuahJvm* jvm, const char* method_name) {
  if (!jvm || !method_name) return 0;
  return dispatch_game_void(jvm, method_name, "(J)V", method_name, [&](jmethodID m) { jvm->env->CallVoidMethod(jvm->activity, m, jvm->native_handle); });
}
extern "C" int nuah_jvm_dispatch_window_focus(NuahJvm* jvm, int has_focus) {
  if (!jvm || !jvm->activity) return 0;
  /* Use one normal virtual dispatch, exactly like Android's Activity window manager. Do not call ATL's standalone activity_window_ready() here:. */
  return dispatch_game_void(jvm, "onWindowFocusChanged", "(Z)V", "onWindowFocusChanged", [&](jmethodID m) { jvm->env->CallVoidMethod(jvm->activity, m, has_focus ? JNI_TRUE : JNI_FALSE); });
}
extern "C" int nuah_jvm_dispatch_surface_created(NuahJvm* jvm, void* surface) {
  if (!jvm || !surface) return 0;
  return dispatch_game_void(jvm, "onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", "onSurfaceCreatedNative", [&](jmethodID m) { jvm->env->CallVoidMethod(jvm->activity, m, jvm->native_handle, static_cast<jobject>(surface)); });
}
extern "C" int nuah_jvm_dispatch_surface_changed(NuahJvm* jvm, void* surface, int format, int width, int height) {
  if (!jvm || !surface || width <= 0 || height <= 0) return 0;
  return dispatch_game_void(jvm, "onSurfaceChangedNative", "(JLandroid/view/Surface;III)V", "onSurfaceChangedNative", [&](jmethodID m) { jvm->env->CallVoidMethod(jvm->activity, m, jvm->native_handle, static_cast<jobject>(surface), format, width, height); });
}
extern "C" int nuah_jvm_dispatch_surface_view_lifecycle(NuahJvm* jvm, int width, int height) {
  if (!jvm || !jvm->activity || width <= 0 || height <= 0) return 0;
  jclass game_activity = find_class(jvm->env, "com/google/androidgamesdk/GameActivity");
  if (!game_activity) return 0;
  const jfieldID view_field = jvm->env->GetFieldID(game_activity, "H", "Lcom/google/androidgamesdk/GameActivity$d;");
  jobject view = view_field ? jvm->env->GetObjectField(jvm->activity, view_field) : nullptr;
  clear_exception(jvm->env, "GameActivity SurfaceView field");
  if (!view) return 0;
  jclass surface_view = find_class(jvm->env, "android/view/SurfaceView");
  const jmethodID dispatch = surface_view ? jvm->env->GetMethodID(surface_view, "dispatchSurfaceLifecycle", "(II)V") : nullptr;
  if (env_flag("NUAH_SURFACE_VIEW_CALLBACK_DIRECT") && surface_view) {
    /* The installed ATL Java façade can enqueue View.post(), but its UI queue is not guaranteed to drain while the native launch call is. */
    const jmethodID created = jvm->env->GetMethodID(surface_view, "surfaceCreated", "()V"); const jmethodID changed = jvm->env->GetMethodID(surface_view, "surfaceChanged", "(III)V");
    if (created && changed) {
      jvm->env->CallVoidMethod(view, created); jvm->env->CallVoidMethod(view, changed, 1, width, height);
      if (check_clear(jvm->env, "SurfaceView direct lifecycle call")) return 0;
      art_trace("nuah ART: dispatched SurfaceView lifecycle directly %dx%d\n", width, height);
      return 1;
    }
    clear_exception(jvm->env, "SurfaceView direct lifecycle lookup");
  }
  if (!dispatch) { clear_exception(jvm->env, "SurfaceView.dispatchSurfaceLifecycle"); return 0; }
  jvm->env->CallVoidMethod(view, dispatch, width, height);
  if (check_clear(jvm->env, "SurfaceView.dispatchSurfaceLifecycle call")) return 0;
  art_trace("nuah ART: queued SurfaceView lifecycle %dx%d\n", width, height);
  return 1;
}
extern "C" int nuah_jvm_dispatch_surface_destroyed(NuahJvm* jvm, void* surface) {
  (void)surface;
  if (!jvm) return 0;
  return dispatch_game_void(jvm, "onSurfaceDestroyedNative", "(J)V", "onSurfaceDestroyedNative", [&](jmethodID m) { jvm->env->CallVoidMethod(jvm->activity, m, jvm->native_handle); });
}
extern "C" int nuah_jvm_dispatch_key(NuahJvm* jvm, int keycode, int action, int repeat, int scancode, unsigned int modifiers, unsigned long long event_time_ms) {
  if (!jvm) return 0;
  const bool key_down = action != 0;
  const char* activity_name = key_down ? "onKeyDown" : "onKeyUp";
  const char* native_name = key_down ? "onKeyDownNative" : "onKeyUpNative";
  const int android_action = key_down ? 0 : 1;
  input_trace("nuah input: keycode=%d action=%s android_action=%d scancode=%d repeat=%d modifiers=0x%x\n", keycode, activity_name, android_action, scancode, repeat, modifiers);
  /* Sober does not inject directly into GameActivity's native method. Its Android path sends the event through MainGameActivity.onKeyDown/Up.. */
  jmethodID activity_method = NUAH_ROBLOX_METHOD(jvm->env, activity_name, "(ILandroid/view/KeyEvent;)Z"); jmethodID method = NUAH_GAME_METHOD(jvm->env, native_name, "(JLandroid/view/KeyEvent;)Z");
  jobject event = static_cast<jobject>(nuah_jvm_key_event(jvm, keycode, android_action, repeat, scancode, modifiers, event_time_ms));
  if (!event) return 0;
  /* This is the actual Sober desktop-keyboard contract, recovered from the APK's kl.g helper: nativePassKeyEvent(down, event.getScanCode(),. */
  if (is_gameplay_key(keycode) && pass_key_to_engine(jvm, key_down, scancode, keycode, repeat ? JNI_TRUE : JNI_FALSE, "NativeGLInterface gameplay key")) { input_trace("nuah input: Roblox key dispatched keycode=%d scancode=%d down=%d repeat=%d\n", keycode, scancode, key_down ? 1 : 0, repeat ? 1 : 0); return 1; }
  if (activity_method) {
    const jboolean result = jvm->env->CallBooleanMethod(jvm->activity, activity_method, keycode, event);
    if (!check_clear(jvm->env, activity_name)) { input_trace("nuah input: %s result=%d\n", activity_name, result == JNI_TRUE ? 1 : 0); if (result == JNI_TRUE) return 1; }
  }
  /* Sober's hardware-keyboard branch calls this static native directly from MainGameActivity. If the façade's Configuration did not make that. */
  if (pass_key_to_engine(jvm, key_down, scancode, keycode, repeat ? JNI_TRUE : JNI_FALSE, "NativeGLInterface.nativePassKeyEvent")) { input_trace("nuah input: NativeGLInterface.nativePassKeyEvent direct\n"); return 1; }
  if (!method) return 0;
  const jboolean result = jvm->env->CallBooleanMethod(jvm->activity, method, jvm->native_handle, event);
  if (check_clear(jvm->env, native_name)) return 0;
  input_trace("nuah input: %s fallback result=%d\n", native_name, result == JNI_TRUE ? 1 : 0);
  return result == JNI_TRUE;
}
extern "C" int nuah_jvm_dispatch_pointer(NuahJvm* jvm, int pointer_type, int action, int button, double x, double y, double dx, double dy, unsigned long long event_time_ms) {
  if (!jvm) return 0;
  /* This is the path used by Sober's il.e adapter for a desktop mouse. The previous Nuah implementation converted every pointer event into a. */
  if (ensure_native_input_methods(jvm)) {
    /* input_bridge.cpp already queries Roblox's mouse-lock callback before each motion/wheel event and synchronizes SDL relative mode from that. */
    bool dispatched = false;
    if (pointer_type == NUAH_POINTER_MOTION && jvm->native_pass_mouse_move) {
      jvm->env->CallStaticVoidMethod(jvm->native_input_interface, jvm->native_pass_mouse_move, static_cast<jfloat>(x), static_cast<jfloat>(y), static_cast<jfloat>(dx), static_cast<jfloat>(dy));
      dispatched = true;
    } else if (pointer_type == NUAH_POINTER_BUTTON && jvm->native_pass_mouse_button) {
      /* Sober passes MotionEvent.actionButton - 1. SDL numbers buttons as 1=left, 2=middle, 3=right, while Android uses button bit values. */
      const jint native_button = sdl_button_to_native(button);
      jvm->env->CallStaticVoidMethod(jvm->native_input_interface, jvm->native_pass_mouse_button, static_cast<jfloat>(x), static_cast<jfloat>(y), action != 0 ? JNI_TRUE : JNI_FALSE, native_button);
      dispatched = true;
    } else if (pointer_type == NUAH_POINTER_WHEEL && jvm->native_pass_mouse_wheel) {
      /* SDL reports vertical wheel movement in dy, matching Android's AXIS_VSCROLL value used by Sober. Preserve horizontal wheel input if the. */
      const double amount = dy != 0.0 ? dy : dx;
      jvm->env->CallStaticVoidMethod(jvm->native_input_interface, jvm->native_pass_mouse_wheel, static_cast<jfloat>(x), static_cast<jfloat>(y), static_cast<jfloat>(amount));
      dispatched = true;
    }
    if (dispatched) {
      if (!check_clear(jvm->env, "NativeInputInterface mouse dispatch")) { input_trace("nuah input: mouse type=%d action=%d button=%d x=%.2f y=%.2f dx=%.2f dy=%.2f\n", pointer_type, action, button, x, y, dx, dy); return 1; }
    }
  }
  jmethodID method = NUAH_GAME_METHOD(jvm->env, "onTouchEventNative", "(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z");
  jobject event = static_cast<jobject>(nuah_jvm_motion_event(jvm, action, button, x, y, dx, dy, event_time_ms));
  if (!method || !event) return 0;
  /* GameActivity's native contract mirrors MotionEvent.dispatch: the five integer fields are pointerCount, historySize, deviceId, source,. */
  constexpr jint kPointerCount = 1, kHistorySize = 0, kDeviceId = 0;
  constexpr jint kSource = 0x1002;  // SOURCE_TOUCHSCREEN, ATL's pointer path
  const jint action_button = action == 0 || action == 1 ? button : 0;
  const jint button_state = button;
  const jboolean result = jvm->env->CallBooleanMethod(jvm->activity, method, jvm->native_handle, event, kPointerCount, kHistorySize, kDeviceId, kSource, action, static_cast<jlong>(event_time_ms),
                                                      static_cast<jlong>(event_time_ms), 0, 0, action_button, button_state, 0, 0, 1.0f, 1.0f);
  if (check_clear(jvm->env, "onTouchEventNative")) return 0;
  return result == JNI_TRUE;
}
extern "C" int nuah_jvm_is_mouse_locked_center(NuahJvm* jvm) {
  if (!jvm || !ensure_native_input_methods(jvm) || !jvm->native_get_mouse_locked_center) return -1;
  const jboolean locked = jvm->env->CallStaticBooleanMethod(jvm->native_input_interface, jvm->native_get_mouse_locked_center);
  if (check_clear(jvm->env, "NativeInputInterface mouse-lock query")) return -1;
  return locked == JNI_TRUE ? 1 : 0;
}
extern "C" int nuah_jvm_dispatch_motion(NuahJvm* jvm, int action, int button, double x, double y, double dx, double dy, unsigned long long event_time_ms) {
  return nuah_jvm_dispatch_pointer(jvm, NUAH_POINTER_MOTION, action, button, x, y, dx, dy, event_time_ms);
}