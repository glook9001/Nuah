#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jvm/jni.h"
static char nuah_content_path[4096], nuah_launch_access_code[4096], nuah_launch_reserved_server_access_code[4096];
static jlong nuah_launch_place_id, nuah_launch_user_id;
static jint nuah_launch_join_request_type;
static jobject nuah_launch_surface;
void nuah_roblox_java_facade_set_content_path(const char* path) { snprintf(nuah_content_path, sizeof(nuah_content_path), "%s", path ? path : ""); }
void nuah_roblox_java_facade_set_launch_place_id(jlong place_id) { nuah_launch_place_id = place_id; }
void nuah_roblox_java_facade_set_start_game_params(const char* access_code, const char* reserved_server_access_code, jlong user_id, jint join_request_type) {
  snprintf(nuah_launch_access_code, sizeof(nuah_launch_access_code), "%s", access_code ? access_code : ""); snprintf(nuah_launch_reserved_server_access_code, sizeof(nuah_launch_reserved_server_access_code), "%s", reserved_server_access_code ? reserved_server_access_code : "");
  nuah_launch_user_id = user_id; nuah_launch_join_request_type = join_request_type;
}
void nuah_roblox_java_facade_set_launch_surface(jobject surface) { nuah_launch_surface = surface; }
jobject com_roblox_engine_jni_NativeGLJavaInterface_getDeviceStaticParams(JNIEnv* env, jclass klass, va_list arguments) {
  (void)klass; (void)arguments; jclass p = (*env)->FindClass(env, "com/roblox/engine/jni/DeviceStaticParams"); return p ? (*env)->AllocObject(env, p) : NULL;
}
static jstring string_value(JNIEnv* env, const char* value) { if (getenv("NUAH_BOOTSTRAP_TRACE")) fprintf(stderr, "nuah facade: String \"%s\"\n", value); return (*env)->NewStringUTF(env, value); }
#define NUAH_DEVSTATIC(ret, name, expr) \
  ret java_lang_Object_##name(JNIEnv* e, jobject o, va_list a) { (void)e; (void)o; (void)a; return (expr); } \
  ret com_roblox_engine_jni_DeviceStaticParams_##name(JNIEnv* e2, jobject o2, va_list a2) { return java_lang_Object_##name(e2, o2, a2); }
NUAH_DEVSTATIC(jobject, osVersion, string_value(e, "36")) NUAH_DEVSTATIC(jobject, deviceName, string_value(e, "Nuah Linux PC")) NUAH_DEVSTATIC(jobject, appVersion, string_value(e, "Roblox")) NUAH_DEVSTATIC(jobject, manufacturer, string_value(e, "Nuah"))
NUAH_DEVSTATIC(jobject, deviceSku, string_value(e, "x86_64")) NUAH_DEVSTATIC(jobject, appBuildVariant, string_value(e, "release")) NUAH_DEVSTATIC(jboolean, cpu64Bit, JNI_TRUE) NUAH_DEVSTATIC(jobject, socModel, string_value(e, "x86_64"))
#undef NUAH_DEVSTATIC
jobject com_roblox_engine_jni_locale_NativeLocaleJavaInterface_getLocale(JNIEnv* env, jclass klass, va_list args) { (void)klass; (void)args; return string_value(env, "en_US"); }
#define NUAH_LOCALE_ALIAS(name) jobject com_roblox_engine_jni_locale_NativeLocaleJavaInterface_##name(JNIEnv* e, jclass k, va_list a) { return com_roblox_engine_jni_locale_NativeLocaleJavaInterface_getLocale(e, k, a); }
NUAH_LOCALE_ALIAS(getRobloxLocale) NUAH_LOCALE_ALIAS(getGameLocale)
#undef NUAH_LOCALE_ALIAS
/* Empty String (not null) for script/variant: null is treated as invalid locale. */
#define NUAH_LOCALE_EMPTY(name) jstring java_util_Locale_##name(JNIEnv* e, jobject o) { (void)o; return string_value(e, ""); }
NUAH_LOCALE_EMPTY(getScript) NUAH_LOCALE_EMPTY(getVariant)
#undef NUAH_LOCALE_EMPTY
#define NUAH_USER(ret, name, expr) ret com_roblox_engine_jni_user_NativeUserJavaInterface_##name(JNIEnv* e, jclass k, va_list a) { (void)e; (void)k; (void)a; return (expr); }
NUAH_USER(jboolean, getHasRobloxSubscription, JNI_FALSE) NUAH_USER(jboolean, getIsUnder13, JNI_FALSE) NUAH_USER(jint, getMembershipType, 0) NUAH_USER(jobject, getAlternateName, string_value(e, "")) NUAH_USER(jobject, getDisplayName, string_value(e, ""))
NUAH_USER(jobject, getPlatformName, string_value(e, "PC")) NUAH_USER(jobject, getTheme, string_value(e, "Dark")) NUAH_USER(jobject, getUsername, string_value(e, "")) NUAH_USER(jlong, getUserId, 0)
#undef NUAH_USER
#define NUAH_CONFIGURATION_INT(name, value) jint android_content_res_Configuration_##name(JNIEnv* e, jobject c) { (void)e; (void)c; return (value); }
NUAH_CONFIGURATION_INT(colorMode, 0) NUAH_CONFIGURATION_INT(densityDpi, 160) NUAH_CONFIGURATION_INT(fontWeightAdjustment, 0) NUAH_CONFIGURATION_INT(hardKeyboardHidden, 1) NUAH_CONFIGURATION_INT(keyboard, 2) NUAH_CONFIGURATION_INT(keyboardHidden, 1) NUAH_CONFIGURATION_INT(mcc, 0) NUAH_CONFIGURATION_INT(mnc, 0) NUAH_CONFIGURATION_INT(navigation, 1)
NUAH_CONFIGURATION_INT(navigationHidden, 1) NUAH_CONFIGURATION_INT(orientation, 2) NUAH_CONFIGURATION_INT(screenHeightDp, 720) NUAH_CONFIGURATION_INT(screenLayout, 2) NUAH_CONFIGURATION_INT(screenWidthDp, 1280) NUAH_CONFIGURATION_INT(smallestScreenWidthDp, 720) NUAH_CONFIGURATION_INT(touchscreen, 3) NUAH_CONFIGURATION_INT(uiMode, 0) /* TOUCHSCREEN_FINGER=3 */
#undef NUAH_CONFIGURATION_INT
#define NUAH_CONFIG_FLOAT(name, expr) jfloat android_content_res_Configuration_##name(JNIEnv* e, jobject c) { (void)e; (void)c; return (expr); }
NUAH_CONFIG_FLOAT(fontScale, 1.0f)
#undef NUAH_CONFIG_FLOAT
static jobject make_value(JNIEnv* env, const char* class_name) { jclass k = (*env)->FindClass(env, class_name); return k ? (*env)->AllocObject(env, k) : NULL; }
#define NUAH_JINT3(fn, val) jint fn(JNIEnv* e, jobject o, va_list a) { (void)e; (void)o; (void)a; return (val); }
#define NUAH_JOBJ_ALLOC(fn, cls) jobject fn(JNIEnv* e, jobject o, va_list a) { (void)o; (void)a; return make_value(e, cls); }
#define NUAH_FORWARD_INT(fn, target) jint fn(JNIEnv* e, jobject o, va_list a) { return target(e, o, a); }
#define NUAH_FORWARD_OBJ(fn, target) jobject fn(JNIEnv* e, jobject o, va_list a) { return target(e, o, a); }
NUAH_JOBJ_ALLOC(android_content_res_Configuration_getLocales, "android/os/LocaleList") NUAH_JINT3(android_os_LocaleList_size, 1) NUAH_JOBJ_ALLOC(android_os_LocaleList_get, "java/util/Locale") NUAH_JINT3(java_util_ArrayList_size, 0)
jobject java_util_ArrayList_get(JNIEnv* env, jobject list, va_list args) { (void)env; (void)list; (void)args; return NULL; }
NUAH_FORWARD_INT(java_util_List_size, java_util_ArrayList_size) NUAH_FORWARD_OBJ(java_util_List_get, java_util_ArrayList_get)
#undef NUAH_JINT3
#undef NUAH_JOBJ_ALLOC
#undef NUAH_FORWARD_INT
#undef NUAH_FORWARD_OBJ
#define NUAH_INIT(ret, name, expr) ret com_roblox_engine_jni_autovalue_AutoValue_InitParams_##name(JNIEnv* e, jobject o, va_list a) { (void)e; (void)o; (void)a; return (expr); }
NUAH_INIT(jobject, baseURL, string_value(e, "https://www.roblox.com/")) NUAH_INIT(jobject, buildVariant, string_value(e, "release")) NUAH_INIT(jobject, userAgent, string_value(e, "Roblox/Android Nuah")) NUAH_INIT(jobject, deviceParams, make_value(e, "com/roblox/engine/jni/model/DeviceParams")) NUAH_INIT(jobject, platformParams, make_value(e, "com/roblox/engine/jni/model/PlatformParams"))
NUAH_INIT(jboolean, isPotato, JNI_FALSE) NUAH_INIT(jboolean, isTablet, JNI_FALSE) NUAH_INIT(jboolean, isVrDevice, JNI_FALSE) NUAH_INIT(jobject, vrContext, NULL)
#undef NUAH_INIT
#define NUAH_DEVICE(ret, name, expr) ret com_roblox_engine_jni_model_DeviceParams_##name(JNIEnv* e, jobject o, va_list a) { (void)e; (void)o; (void)a; return (expr); }
NUAH_DEVICE(jobject, appBuildVariant, string_value(e, "release")) NUAH_DEVICE(jobject, appVersion, string_value(e, "Roblox")) NUAH_DEVICE(jobject, country, string_value(e, "US")) NUAH_DEVICE(jboolean, cpu64Bit, JNI_TRUE) NUAH_DEVICE(jobject, deviceName, string_value(e, "Nuah Linux PC"))
NUAH_DEVICE(jobject, deviceSku, string_value(e, "x86_64")) NUAH_DEVICE(jint, deviceTotalMemoryMB, 4096) NUAH_DEVICE(jint, displayPhysicalHeightPixels, 720) NUAH_DEVICE(jint, displayPhysicalWidthPixels, 1280) NUAH_DEVICE(jobject, displayResolution, string_value(e, "1280x720"))
NUAH_DEVICE(jboolean, isChrome, JNI_FALSE) NUAH_DEVICE(jboolean, isLowRamDevice, JNI_FALSE) NUAH_DEVICE(jint, largeMemoryClass, 512) NUAH_DEVICE(jlong, lowMemoryKillerBackgroundAppThreshold, 0) NUAH_DEVICE(jlong, lowMemoryKillerForegroundAppThreshold, 0)
NUAH_DEVICE(jobject, manufacturer, string_value(e, "Nuah")) NUAH_DEVICE(jint, memoryClass, 256) NUAH_DEVICE(jobject, networkType, string_value(e, "WIFI")) NUAH_DEVICE(jobject, osVersion, string_value(e, "36")) NUAH_DEVICE(jobject, socModel, string_value(e, "x86_64")) NUAH_DEVICE(jobject, testDeviceName, string_value(e, ""))
#undef NUAH_DEVICE
#define NUAH_PLATFORM(ret, name, expr) ret com_roblox_engine_jni_model_PlatformParams_##name(JNIEnv* e, jobject o, va_list a) { (void)e; (void)o; (void)a; return (expr); }
NUAH_PLATFORM(jobject, assetFolderPath, string_value(e, nuah_content_path))
jfloat com_roblox_engine_jni_model_PlatformParams_dpiScale(JNIEnv* env, jobject object, va_list args) { (void)env; (void)object; (void)args; if (getenv("NUAH_BOOTSTRAP_TRACE")) fprintf(stderr, "nuah facade: PlatformParams.dpiScale=1.0\n"); return 1.0f; }
NUAH_PLATFORM(jboolean, isKeyboardDevice, JNI_TRUE) NUAH_PLATFORM(jboolean, isMouseDevice, JNI_TRUE) NUAH_PLATFORM(jboolean, isTouchDevice, JNI_TRUE) NUAH_PLATFORM(jint, viewportHeightMm, 190) NUAH_PLATFORM(jint, viewportWidthMm, 340)
#undef NUAH_PLATFORM
#define NUAH_START(ret, name, expr) ret com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_##name(JNIEnv* e, jobject o, va_list a) { (void)e; (void)o; (void)a; return (expr); }
NUAH_START(jobject, accessCode, string_value(e, nuah_launch_access_code)) NUAH_START(jobject, callId, string_value(e, "")) NUAH_START(jlong, conversationId, 0) NUAH_START(jobject, eventId, string_value(e, "")) NUAH_START(jobject, gameId, string_value(e, ""))
NUAH_START(jobject, gameIdToExclude, string_value(e, "")) NUAH_START(jobject, gameJoinContext, string_value(e, "")) NUAH_START(jboolean, isUnder13, JNI_FALSE) NUAH_START(jobject, isoContext, string_value(e, "")) NUAH_START(jobject, joinAttemptId, string_value(e, "00000000-0000-0000-0000-000000000000"))
NUAH_START(jobject, joinAttemptOrigin, string_value(e, "")) NUAH_START(jint, joinRequestType, nuah_launch_join_request_type) NUAH_START(jobject, launchData, string_value(e, "")) NUAH_START(jobject, linkCode, string_value(e, "")) NUAH_START(jlong, placeId, nuah_launch_place_id)
NUAH_START(jobject, referralPage, string_value(e, "WebView")) NUAH_START(jlong, referredByPlayerId, 0) NUAH_START(jobject, reservedServerAccessCode, string_value(e, nuah_launch_reserved_server_access_code)) NUAH_START(jlong, userId, nuah_launch_user_id)
NUAH_START(jobject, username, string_value(e, (getenv("NUAH_ROBLOX_USERNAME") ? getenv("NUAH_ROBLOX_USERNAME") : ""))) NUAH_START(jobject, surface, nuah_launch_surface) NUAH_START(jobject, platformParams, make_value(e, "com/roblox/engine/jni/model/PlatformParams")) NUAH_START(jobject, deviceParams, NULL) NUAH_START(jobject, vrContext, NULL)
#undef NUAH_START
