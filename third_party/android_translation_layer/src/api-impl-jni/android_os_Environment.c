#include "defines.h"
#include "util.h"

#include "generated_headers/android_os_Environment.h"

char *get_app_data_dir();

JNIEXPORT jstring JNICALL Java_android_os_Environment_native_1get_1app_1data_1dir(JNIEnv *env, jclass this)
{
	return _JSTRING(get_app_data_dir());
}
