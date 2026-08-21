#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jvm/jni.h"

static char nuah_content_path[4096];
static char nuah_launch_access_code[4096];
static char nuah_launch_reserved_server_access_code[4096];
static char nuah_launch_join_attempt_id[80];
static char nuah_launch_join_attempt_origin[64];
static jlong nuah_launch_place_id;
static jlong nuah_launch_user_id;
static jint nuah_launch_join_request_type;
static jobject nuah_launch_surface;

void nuah_roblox_java_facade_set_content_path(const char* path) {
  snprintf(nuah_content_path, sizeof(nuah_content_path), "%s",
           path ? path : "");
}

void nuah_roblox_java_facade_set_launch_place_id(jlong place_id) {
  nuah_launch_place_id = place_id;
}

/* The AutoValue object is read through native accessors in the stripped APK.
 * Keep the values passed to the real Builder visible at that boundary too;
 * otherwise the facade returned empty accessCode/userId values and Roblox
 * fell back to its signed-out account screen instead of joining the room. */
void nuah_roblox_java_facade_set_start_game_params(
    const char* access_code, const char* reserved_server_access_code,
    jlong user_id, jint join_request_type) {
  snprintf(nuah_launch_access_code, sizeof(nuah_launch_access_code), "%s",
           access_code ? access_code : "");
  snprintf(nuah_launch_reserved_server_access_code,
           sizeof(nuah_launch_reserved_server_access_code), "%s",
           reserved_server_access_code ? reserved_server_access_code : "");
  nuah_launch_user_id = user_id;
  nuah_launch_join_request_type = join_request_type;
}

void nuah_roblox_java_facade_set_launch_surface(jobject surface) {
  nuah_launch_surface = surface;
}

void nuah_roblox_java_facade_set_join_attempt(const char* id,
                                              const char* origin) {
  snprintf(nuah_launch_join_attempt_id, sizeof(nuah_launch_join_attempt_id),
           "%s", id ? id : "");
  snprintf(nuah_launch_join_attempt_origin,
           sizeof(nuah_launch_join_attempt_origin), "%s",
           origin ? origin : "");
}

/*
 * Roblox requests this immutable Java value object before registering its
 * GameActivity natives.  Keep it data-only: the JNI core supplies the object
 * identity and these accessors supply truthful host-facing values.
 */
jobject
com_roblox_engine_jni_NativeGLJavaInterface_getDeviceStaticParams(
    JNIEnv* env, jclass klass, va_list arguments) {
  (void)klass;
  (void)arguments;
  jclass params =
      (*env)->FindClass(env, "com/roblox/engine/jni/DeviceStaticParams");
  return params ? (*env)->AllocObject(env, params) : NULL;
}

static jstring string_value(JNIEnv* env, const char* value) {
  if (getenv("NUAH_BOOTSTRAP_TRACE"))
    fprintf(stderr, "nuah facade: String \"%s\"\n", value);
  return (*env)->NewStringUTF(env, value);
}

jobject java_lang_Object_osVersion(JNIEnv* env, jobject object, va_list args) {
  (void)object;
  (void)args;
  return string_value(env, "36");
}

jobject java_lang_Object_deviceName(JNIEnv* env, jobject object, va_list args) {
  (void)object;
  (void)args;
  return string_value(env, "Nuah Linux PC");
}

jobject java_lang_Object_appVersion(JNIEnv* env, jobject object, va_list args) {
  (void)object;
  (void)args;
  return string_value(env, "Roblox");
}

jobject java_lang_Object_manufacturer(JNIEnv* env, jobject object,
                                      va_list args) {
  (void)object;
  (void)args;
  return string_value(env, "Nuah");
}

jobject java_lang_Object_deviceSku(JNIEnv* env, jobject object, va_list args) {
  (void)object;
  (void)args;
  return string_value(env, "x86_64");
}

jobject java_lang_Object_appBuildVariant(JNIEnv* env, jobject object,
                                         va_list args) {
  (void)object;
  (void)args;
  return string_value(env, "release");
}

jboolean java_lang_Object_cpu64Bit(JNIEnv* env, jobject object, va_list args) {
  (void)env;
  (void)object;
  (void)args;
  return JNI_TRUE;
}

jobject java_lang_Object_socModel(JNIEnv* env, jobject object, va_list args) {
  (void)object;
  (void)args;
  return string_value(env, "x86_64");
}

/* DeviceStaticParams is a concrete class, so android2gnulinux resolves its
 * virtual accessors using the concrete JNI symbol prefix.  Keep the generic
 * Object-prefixed helpers above for older callers, but also expose the exact
 * class names used by the current Roblox image. */
#define DEVICE_STATIC_STRING(name, value)                                  \
  jobject com_roblox_engine_jni_DeviceStaticParams_##name(                 \
      JNIEnv* env, jobject object, va_list args) {                          \
    (void)object;                                                           \
    (void)args;                                                             \
    return string_value(env, value);                                        \
  }
#define DEVICE_STATIC_BOOLEAN(name, value)                                 \
  jboolean com_roblox_engine_jni_DeviceStaticParams_##name(                \
      JNIEnv* env, jobject object, va_list args) {                          \
    (void)env;                                                              \
    (void)object;                                                           \
    (void)args;                                                             \
    return value;                                                           \
  }

DEVICE_STATIC_STRING(osVersion, "36")
DEVICE_STATIC_STRING(deviceName, "Nuah Linux PC")
DEVICE_STATIC_STRING(appVersion, "Roblox")
DEVICE_STATIC_STRING(manufacturer, "Nuah")
DEVICE_STATIC_STRING(deviceSku, "x86_64")
DEVICE_STATIC_STRING(appBuildVariant, "release")
DEVICE_STATIC_BOOLEAN(cpu64Bit, JNI_TRUE)
DEVICE_STATIC_STRING(socModel, "x86_64")

#undef DEVICE_STATIC_STRING
#undef DEVICE_STATIC_BOOLEAN

jobject com_roblox_engine_jni_locale_NativeLocaleJavaInterface_getLocale(
    JNIEnv* env, jclass klass, va_list args) {
  (void)klass;
  (void)args;
  return string_value(env, "en_US");
}

jobject
com_roblox_engine_jni_locale_NativeLocaleJavaInterface_getRobloxLocale(
    JNIEnv* env, jclass klass, va_list args) {
  (void)klass;
  (void)args;
  return string_value(env, "en_US");
}

/* The locale bridge asks for the game locale immediately after the Roblox
 * locale.  Keep both values non-null until the authenticated WebKit session
 * supplies a server locale. */
jobject
com_roblox_engine_jni_locale_NativeLocaleJavaInterface_getGameLocale(
    JNIEnv* env, jclass klass, va_list args) {
  (void)klass;
  (void)args;
  return string_value(env, "en_US");
}

/* Android's Locale exposes these accessors even when the script/variant
 * portion is empty.  Returning an empty String is important: Roblox asks for
 * getScript() while constructing the API-36 Configuration and treats a null
 * result as an invalid locale. */
jstring java_util_Locale_getScript(JNIEnv* env, jobject object) {
  (void)object;
  return string_value(env, "");
}

jstring java_util_Locale_getVariant(JNIEnv* env, jobject object) {
  (void)object;
  return string_value(env, "");
}

/*
 * MainGameActivity installs this user snapshot before native app startup.
 * Authentication is connected later; until then provide a complete, typed
 * anonymous snapshot so libroblox never receives null for a Java String.
 */
#define USER_STRING(name, value)                                        \
  jobject com_roblox_engine_jni_user_NativeUserJavaInterface_##name(    \
      JNIEnv* env, jclass klass, va_list args) {                         \
    (void)klass;                                                        \
    (void)args;                                                         \
    return string_value(env, value);                                    \
  }
#define USER_BOOLEAN(name, value)                                       \
  jboolean com_roblox_engine_jni_user_NativeUserJavaInterface_##name(   \
      JNIEnv* env, jclass klass, va_list args) {                         \
    (void)env;                                                          \
    (void)klass;                                                        \
    (void)args;                                                         \
    return value;                                                       \
  }
#define USER_INT(name, value)                                           \
  jint com_roblox_engine_jni_user_NativeUserJavaInterface_##name(       \
      JNIEnv* env, jclass klass, va_list args) {                         \
    (void)env;                                                          \
    (void)klass;                                                        \
    (void)args;                                                         \
    return value;                                                       \
  }
#define USER_LONG(name, value)                                          \
  jlong com_roblox_engine_jni_user_NativeUserJavaInterface_##name(      \
      JNIEnv* env, jclass klass, va_list args) {                         \
    (void)env;                                                          \
    (void)klass;                                                        \
    (void)args;                                                         \
    return value;                                                       \
  }

USER_BOOLEAN(getHasRobloxSubscription, JNI_FALSE)
USER_BOOLEAN(getIsUnder13, JNI_FALSE)
USER_INT(getMembershipType, 0)
USER_STRING(getAlternateName, "")
USER_STRING(getDisplayName, "")
USER_STRING(getPlatformName, "PC")
USER_STRING(getTheme, "Dark")
USER_STRING(getUsername, "")
USER_LONG(getUserId, 0)

/* API-36 Configuration values observed by GameActivity_register. */
#define NUAH_CONFIGURATION_INT(name, value)                              \
  jint android_content_res_Configuration_##name(JNIEnv* env,             \
                                                 jobject configuration) { \
    (void)env;                                                           \
    (void)configuration;                                                 \
    return value;                                                        \
  }

NUAH_CONFIGURATION_INT(colorMode, 0)
NUAH_CONFIGURATION_INT(densityDpi, 160)
NUAH_CONFIGURATION_INT(fontWeightAdjustment, 0)
NUAH_CONFIGURATION_INT(hardKeyboardHidden, 1)
NUAH_CONFIGURATION_INT(keyboard, 2)
NUAH_CONFIGURATION_INT(keyboardHidden, 1)
NUAH_CONFIGURATION_INT(mcc, 0)
NUAH_CONFIGURATION_INT(mnc, 0)
NUAH_CONFIGURATION_INT(navigation, 1)
NUAH_CONFIGURATION_INT(navigationHidden, 1)
NUAH_CONFIGURATION_INT(orientation, 2)
NUAH_CONFIGURATION_INT(screenHeightDp, 720)
NUAH_CONFIGURATION_INT(screenLayout, 2)
NUAH_CONFIGURATION_INT(screenWidthDp, 1280)
NUAH_CONFIGURATION_INT(smallestScreenWidthDp, 720)
/* Android Configuration.TOUCHSCREEN_FINGER (3). NOTOUCH (1) suppresses the
 * Roblox Movement Mode selector even when PlatformParams advertises a hybrid
 * keyboard/mouse/touch device. */
NUAH_CONFIGURATION_INT(touchscreen, 3)
NUAH_CONFIGURATION_INT(uiMode, 0)

jfloat android_content_res_Configuration_fontScale(
    JNIEnv* env, jobject configuration) {
  (void)env;
  (void)configuration;
  return 1.0f;
}

jobject android_content_res_Configuration_getLocales(
    JNIEnv* env, jobject configuration, va_list args) {
  (void)configuration;
  (void)args;
  jclass klass = (*env)->FindClass(env, "android/os/LocaleList");
  return klass ? (*env)->AllocObject(env, klass) : NULL;
}

jint android_os_LocaleList_size(JNIEnv* env, jobject list, va_list args) {
  (void)env;
  (void)list;
  (void)args;
  return 1;
}

jobject android_os_LocaleList_get(JNIEnv* env, jobject list, va_list args) {
  (void)list;
  (void)args;
  jclass klass = (*env)->FindClass(env, "java/util/Locale");
  return klass ? (*env)->AllocObject(env, klass) : NULL;
}

/* API 30+ returns an empty java.util.List when there are no historical
 * ApplicationExitInfo records.  Roblox consumes that list during
 * nativePostClientSettingsLoadedInitialization3.  Keep the boundary typed
 * and empty rather than passing NULL or inventing exit records. */
jint java_util_ArrayList_size(JNIEnv* env, jobject list, va_list args) {
  (void)env;
  (void)list;
  (void)args;
  return 0;
}

jobject java_util_ArrayList_get(JNIEnv* env, jobject list, va_list args) {
  (void)env;
  (void)list;
  (void)args;
  return NULL;
}

jint java_util_List_size(JNIEnv* env, jobject list, va_list args) {
  return java_util_ArrayList_size(env, list, args);
}

jobject java_util_List_get(JNIEnv* env, jobject list, va_list args) {
  return java_util_ArrayList_get(env, list, args);
}

static jobject make_value(JNIEnv* env, const char* class_name) {
  jclass klass = (*env)->FindClass(env, class_name);
  return klass ? (*env)->AllocObject(env, klass) : NULL;
}

#define INIT_PARAMS_OBJECT(name, class_name)                              \
  jobject com_roblox_engine_jni_autovalue_AutoValue_InitParams_##name(   \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)object;                                                        \
    (void)args;                                                          \
    return make_value(env, class_name);                                  \
  }

#define INIT_PARAMS_STRING(name, value)                                  \
  jobject com_roblox_engine_jni_autovalue_AutoValue_InitParams_##name(   \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)object;                                                        \
    (void)args;                                                          \
    return string_value(env, value);                                     \
  }

#define INIT_PARAMS_BOOLEAN(name, value)                                 \
  jboolean com_roblox_engine_jni_autovalue_AutoValue_InitParams_##name(  \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    return value;                                                        \
  }

INIT_PARAMS_STRING(baseURL, "https://www.roblox.com/")
INIT_PARAMS_STRING(buildVariant, "release")
INIT_PARAMS_STRING(userAgent, "Roblox/Android Nuah")
INIT_PARAMS_OBJECT(deviceParams, "com/roblox/engine/jni/model/DeviceParams")
INIT_PARAMS_OBJECT(platformParams,
                   "com/roblox/engine/jni/model/PlatformParams")
INIT_PARAMS_BOOLEAN(isPotato, JNI_FALSE)
INIT_PARAMS_BOOLEAN(isTablet, JNI_FALSE)
INIT_PARAMS_BOOLEAN(isVrDevice, JNI_FALSE)

jobject com_roblox_engine_jni_autovalue_AutoValue_InitParams_vrContext(
    JNIEnv* env, jobject object, va_list args) {
  (void)env;
  (void)object;
  (void)args;
  return NULL;
}

#define DEVICE_STRING(name, value)                                      \
  jobject com_roblox_engine_jni_model_DeviceParams_##name(              \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)object;                                                        \
    (void)args;                                                          \
    if (getenv("NUAH_BOOTSTRAP_TRACE"))                                  \
      fprintf(stderr, "nuah facade: DeviceParams.%s\\n", #name);       \
    return string_value(env, value);                                    \
  }
#define DEVICE_INT(name, value)                                         \
  jint com_roblox_engine_jni_model_DeviceParams_##name(                 \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    if (getenv("NUAH_BOOTSTRAP_TRACE"))                                  \
      fprintf(stderr, "nuah facade: DeviceParams.%s=%d\\n", #name,     \
              (int)(value));                                             \
    return value;                                                        \
  }
#define DEVICE_LONG(name, value)                                        \
  jlong com_roblox_engine_jni_model_DeviceParams_##name(                \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    if (getenv("NUAH_BOOTSTRAP_TRACE"))                                  \
      fprintf(stderr, "nuah facade: DeviceParams.%s=%lld\\n", #name,   \
              (long long)(value));                                       \
    return value;                                                        \
  }
#define DEVICE_BOOLEAN(name, value)                                     \
  jboolean com_roblox_engine_jni_model_DeviceParams_##name(             \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    if (getenv("NUAH_BOOTSTRAP_TRACE"))                                  \
      fprintf(stderr, "nuah facade: DeviceParams.%s=%d\\n", #name,     \
              (int)(value));                                             \
    return value;                                                        \
  }

DEVICE_STRING(appBuildVariant, "release")
DEVICE_STRING(appVersion, "Roblox")
DEVICE_STRING(country, "US")
DEVICE_BOOLEAN(cpu64Bit, JNI_TRUE)
DEVICE_STRING(deviceName, "Nuah Linux PC")
DEVICE_STRING(deviceSku, "x86_64")
DEVICE_INT(deviceTotalMemoryMB, 4096)
DEVICE_INT(displayPhysicalHeightPixels, 720)
DEVICE_INT(displayPhysicalWidthPixels, 1280)
DEVICE_STRING(displayResolution, "1280x720")
DEVICE_BOOLEAN(isChrome, JNI_FALSE)
DEVICE_BOOLEAN(isLowRamDevice, JNI_FALSE)
DEVICE_INT(largeMemoryClass, 512)
DEVICE_LONG(lowMemoryKillerBackgroundAppThreshold, 0)
DEVICE_LONG(lowMemoryKillerForegroundAppThreshold, 0)
DEVICE_STRING(manufacturer, "Nuah")
DEVICE_INT(memoryClass, 256)
DEVICE_STRING(networkType, "WIFI")
DEVICE_STRING(osVersion, "36")
DEVICE_STRING(socModel, "x86_64")
DEVICE_STRING(testDeviceName, "")

jobject com_roblox_engine_jni_model_PlatformParams_assetFolderPath(
    JNIEnv* env, jobject object, va_list args) {
  (void)object;
  (void)args;
  return string_value(env, nuah_content_path);
}

#define PLATFORM_BOOLEAN(name, value)                                   \
  jboolean com_roblox_engine_jni_model_PlatformParams_##name(           \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    if (getenv("NUAH_BOOTSTRAP_TRACE"))                                  \
      fprintf(stderr, "nuah facade: PlatformParams.%s=%d\n", #name,     \
              (int)(value));                                             \
    return value;                                                        \
  }
#define PLATFORM_INT(name, value)                                       \
  jint com_roblox_engine_jni_model_PlatformParams_##name(               \
      JNIEnv* env, jobject object, va_list args) {                        \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    if (getenv("NUAH_BOOTSTRAP_TRACE"))                                  \
      fprintf(stderr, "nuah facade: PlatformParams.%s=%d\n", #name,     \
              (int)(value));                                             \
    return value;                                                        \
  }

jfloat com_roblox_engine_jni_model_PlatformParams_dpiScale(
    JNIEnv* env, jobject object, va_list args) {
  (void)env;
  (void)object;
  (void)args;
  if (getenv("NUAH_BOOTSTRAP_TRACE"))
    fprintf(stderr, "nuah facade: PlatformParams.dpiScale=1.0\n");
  return 1.0f;
}

PLATFORM_BOOLEAN(isKeyboardDevice, JNI_TRUE)
PLATFORM_BOOLEAN(isMouseDevice, JNI_TRUE)
/* Sober presents a hybrid Android input surface: Roblox can see the
 * physical keyboard/mouse while the Android settings still expose the
 * movement-mode selector.  Keeping TouchEnabled true is the capability
 * advertisement; it does not synthesize touch events, which continue to be
 * absent unless SDL reports them. */
PLATFORM_BOOLEAN(isTouchDevice, JNI_TRUE)
PLATFORM_INT(viewportHeightMm, 190)
PLATFORM_INT(viewportWidthMm, 340)

/* The real Java GameManager builds this immutable value before it calls
 * NativeGLInterface.nativeAppBridgeV2StartGameWithParam.  Nuah invokes that
 * same exported JNI entry point directly, so expose the small immutable
 * accessor surface it reads instead of manufacturing another Java runtime. */
#define START_GAME_STRING(name, value)                                  \
  jobject com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_##name(\
      JNIEnv* env, jobject object, va_list args) {                       \
    (void)object;                                                        \
    (void)args;                                                          \
    return string_value(env, value);                                     \
  }
#define START_GAME_LONG(name, expression)                               \
  jlong com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_##name(\
      JNIEnv* env, jobject object, va_list args) {                       \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    return (expression);                                                 \
  }
#define START_GAME_INT(name, value)                                     \
  jint com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_##name( \
      JNIEnv* env, jobject object, va_list args) {                       \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    return (value);                                                       \
  }
#define START_GAME_BOOLEAN(name, value)                                  \
  jboolean com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_##name(\
      JNIEnv* env, jobject object, va_list args) {                       \
    (void)env;                                                           \
    (void)object;                                                        \
    (void)args;                                                          \
    return (value);                                                       \
  }

START_GAME_STRING(accessCode, nuah_launch_access_code)
START_GAME_STRING(callId, "")
START_GAME_LONG(conversationId, 0)
START_GAME_STRING(eventId, "")
START_GAME_STRING(gameId, "")
START_GAME_STRING(gameIdToExclude, "")
START_GAME_STRING(gameJoinContext, "")
START_GAME_BOOLEAN(isUnder13, JNI_FALSE)
START_GAME_STRING(isoContext, "")
START_GAME_STRING(joinAttemptId, nuah_launch_join_attempt_id)
START_GAME_STRING(joinAttemptOrigin, nuah_launch_join_attempt_origin)
/* vi.j0.a(placeId, null, ...) uses request type 2 for a WebView launch. */
START_GAME_INT(joinRequestType, nuah_launch_join_request_type)
START_GAME_STRING(launchData, "")
START_GAME_STRING(linkCode, "")
START_GAME_LONG(placeId, nuah_launch_place_id)
START_GAME_STRING(referralPage, "WebView")
START_GAME_LONG(referredByPlayerId, 0)
START_GAME_STRING(reservedServerAccessCode, nuah_launch_reserved_server_access_code)
START_GAME_LONG(userId, nuah_launch_user_id)
START_GAME_STRING(username, (getenv("NUAH_ROBLOX_USERNAME") ? getenv("NUAH_ROBLOX_USERNAME") : ""))

jobject com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_surface(
    JNIEnv* env, jobject object, va_list args) {
  (void)env;
  (void)object;
  (void)args;
  return nuah_launch_surface;
}

jobject com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_platformParams(
    JNIEnv* env, jobject object, va_list args) {
  (void)object;
  (void)args;
  return make_value(env, "com/roblox/engine/jni/model/PlatformParams");
}

jobject com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_deviceParams(
    JNIEnv* env, jobject object, va_list args) {
  (void)env;
  (void)object;
  (void)args;
  return NULL;
}

jobject com_roblox_engine_jni_autovalue_AutoValue_StartGameParams_vrContext(
    JNIEnv* env, jobject object, va_list args) {
  (void)env;
  (void)object;
  (void)args;
  return NULL;
}

#undef START_GAME_STRING
#undef START_GAME_LONG
#undef START_GAME_INT
#undef START_GAME_BOOLEAN
