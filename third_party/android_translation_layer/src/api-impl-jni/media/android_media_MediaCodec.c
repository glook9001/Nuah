/*
 * Android MediaCodec stub implementation for Android Translation Layer
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../defines.h"
#include "../util.h"
#include "../generated_headers/android_media_MediaCodec.h"
#include "jni.h"

struct ATL_codec_context {
	int dummy;
};

JNIEXPORT jlong JNICALL Java_android_media_MediaCodec_native_1constructor(JNIEnv *env, jobject this, jstring codec_name)
{
	(void)env; (void)this; (void)codec_name;
	struct ATL_codec_context *ctx = calloc(1, sizeof(struct ATL_codec_context));
	return _INTPTR(ctx);
}

JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1configure_1audio(JNIEnv *env, jobject this, jlong codec, jobject extradata, jint sample_rate, jint nb_channels)
{
	(void)env; (void)this; (void)codec; (void)extradata; (void)sample_rate; (void)nb_channels;
}

JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1configure_1video(JNIEnv *env, jobject this, jlong codec, jobject extradata_audio, jobject extradata_video, jobject surface)
{
	(void)env; (void)this; (void)codec; (void)extradata_audio; (void)extradata_video; (void)surface;
}

JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1start(JNIEnv *env, jobject this, jlong codec)
{
	(void)env; (void)this; (void)codec;
}

JNIEXPORT jint JNICALL Java_android_media_MediaCodec_native_1queueInputBuffer(JNIEnv *env, jobject this, jlong codec, jobject buffer, jlong pts)
{
	(void)env; (void)this; (void)codec; (void)buffer; (void)pts;
	return 0;
}

JNIEXPORT jint JNICALL Java_android_media_MediaCodec_native_1dequeueOutputBuffer(JNIEnv *env, jobject this, jlong codec, jobject buffer, jobject info)
{
	(void)env; (void)this; (void)codec; (void)buffer; (void)info;
	return -1; // INFO_TRY_AGAIN_LATER
}

JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1releaseOutputBuffer(JNIEnv *env, jobject this, jlong codec, jobject buffer, jboolean render)
{
	(void)env; (void)this; (void)codec; (void)buffer; (void)render;
}

JNIEXPORT void JNICALL Java_android_media_MediaCodec_native_1release(JNIEnv *env, jobject this, jlong codec)
{
	(void)env; (void)this;
	struct ATL_codec_context *ctx = _PTR(codec);
	free(ctx);
}
