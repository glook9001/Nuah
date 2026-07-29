#pragma once

#include <jni.h>

/* Invoke an Android library's JNI_OnLoad against the helper-owned JNI ABI. */
int nuah_jni_invoke_onload(void* module);
