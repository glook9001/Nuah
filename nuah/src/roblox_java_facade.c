#include <stdarg.h>
#include <string.h>

#include "jvm/jni.h"

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
