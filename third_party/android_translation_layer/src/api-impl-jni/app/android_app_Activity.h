#include <jni.h>

void activity_start(JNIEnv *env, jobject activity_object);
jobject atl_current_activity(JNIEnv *env);
void activity_window_ready(void);
void activity_close_all(void);
void current_activity_back_pressed(void);
