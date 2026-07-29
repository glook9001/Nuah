#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jvm/jni.h"

static char nuah_content_path[4096];

void nuah_roblox_java_facade_set_content_path(const char* path) {
  snprintf(nuah_content_path, sizeof(nuah_content_path), "%s",
           path ? path : "");
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
  return string_value(env, "16");
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

jobject com_roblox_engine_jni_locale_NativeLocaleJavaInterface_getLocale(
    JNIEnv* env, jclass klass, va_list args) {
  (void)klass;
  (void)args;
  return string_value(env, "en_US");
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
NUAH_CONFIGURATION_INT(touchscreen, 1)
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
      JNIEnv* env, jobject object) {                                     \
    (void)object;                                                        \
    return string_value(env, value);                                    \
  }
#define DEVICE_INT(name, value)                                         \
  jint com_roblox_engine_jni_model_DeviceParams_##name(                 \
      JNIEnv* env, jobject object) {                                     \
    (void)env;                                                           \
    (void)object;                                                        \
    return value;                                                        \
  }
#define DEVICE_LONG(name, value)                                        \
  jlong com_roblox_engine_jni_model_DeviceParams_##name(                \
      JNIEnv* env, jobject object) {                                     \
    (void)env;                                                           \
    (void)object;                                                        \
    return value;                                                        \
  }
#define DEVICE_BOOLEAN(name, value)                                     \
  jboolean com_roblox_engine_jni_model_DeviceParams_##name(             \
      JNIEnv* env, jobject object) {                                     \
    (void)env;                                                           \
    (void)object;                                                        \
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
DEVICE_STRING(osVersion, "16")
DEVICE_STRING(socModel, "x86_64")
DEVICE_STRING(testDeviceName, "")

jobject com_roblox_engine_jni_model_PlatformParams_assetFolderPath(
    JNIEnv* env, jobject object) {
  (void)object;
  return string_value(env, nuah_content_path);
}

#define PLATFORM_BOOLEAN(name, value)                                   \
  jboolean com_roblox_engine_jni_model_PlatformParams_##name(           \
      JNIEnv* env, jobject object) {                                     \
    (void)env;                                                           \
    (void)object;                                                        \
    if (getenv("NUAH_BOOTSTRAP_TRACE"))                                  \
      fprintf(stderr, "nuah facade: PlatformParams.%s=%d\n", #name,     \
              (int)(value));                                             \
    return value;                                                        \
  }
#define PLATFORM_INT(name, value)                                       \
  jint com_roblox_engine_jni_model_PlatformParams_##name(               \
      JNIEnv* env, jobject object) {                                     \
    (void)env;                                                           \
    (void)object;                                                        \
    if (getenv("NUAH_BOOTSTRAP_TRACE"))                                  \
      fprintf(stderr, "nuah facade: PlatformParams.%s=%d\n", #name,     \
              (int)(value));                                             \
    return value;                                                        \
  }

jfloat com_roblox_engine_jni_model_PlatformParams_dpiScale(
    JNIEnv* env, jobject object) {
  (void)env;
  (void)object;
  if (getenv("NUAH_BOOTSTRAP_TRACE"))
    fprintf(stderr, "nuah facade: PlatformParams.dpiScale=1.0\n");
  return 1.0f;
}

PLATFORM_BOOLEAN(isKeyboardDevice, JNI_TRUE)
PLATFORM_BOOLEAN(isMouseDevice, JNI_TRUE)
PLATFORM_BOOLEAN(isTouchDevice, JNI_FALSE)
PLATFORM_INT(viewportHeightMm, 190)
PLATFORM_INT(viewportWidthMm, 340)
