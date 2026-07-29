#include <stdint.h>

/* Android-ELF provider boundary.  Each output DSO has its Android SONAME and
 * is loaded only by linker64; no host-glibc provider enters this namespace. */
#define PLATFORM_STUB(name) void name(void) {}

PLATFORM_STUB(AAssetManager_fromJava) PLATFORM_STUB(AAssetManager_open)
PLATFORM_STUB(AAsset_close) PLATFORM_STUB(AAsset_getBuffer)
PLATFORM_STUB(AAsset_getLength) PLATFORM_STUB(AAsset_openFileDescriptor)
PLATFORM_STUB(AConfiguration_delete) PLATFORM_STUB(AConfiguration_fromAssetManager)
PLATFORM_STUB(AConfiguration_getCountry) PLATFORM_STUB(AConfiguration_getLanguage)
PLATFORM_STUB(AConfiguration_getNavHidden) PLATFORM_STUB(AConfiguration_getScreenHeightDp)
PLATFORM_STUB(AConfiguration_getScreenSize) PLATFORM_STUB(AConfiguration_getScreenWidthDp)
PLATFORM_STUB(AConfiguration_new) PLATFORM_STUB(ALooper_acquire)
PLATFORM_STUB(ALooper_addFd) PLATFORM_STUB(ALooper_forThread)
PLATFORM_STUB(ALooper_pollOnce) PLATFORM_STUB(ALooper_prepare)
PLATFORM_STUB(ALooper_release) PLATFORM_STUB(ALooper_removeFd)
PLATFORM_STUB(AMediaCodec_configure) PLATFORM_STUB(AMediaCodec_createDecoderByType)
PLATFORM_STUB(AMediaCodec_createEncoderByType) PLATFORM_STUB(AMediaCodec_delete)
PLATFORM_STUB(AMediaCodec_dequeueInputBuffer) PLATFORM_STUB(AMediaCodec_dequeueOutputBuffer)
PLATFORM_STUB(AMediaCodec_flush) PLATFORM_STUB(AMediaCodec_getInputBuffer)
PLATFORM_STUB(AMediaCodec_getOutputBuffer) PLATFORM_STUB(AMediaCodec_getOutputFormat)
PLATFORM_STUB(AMediaCodec_queueInputBuffer) PLATFORM_STUB(AMediaCodec_releaseOutputBuffer)
PLATFORM_STUB(AMediaCodec_start) PLATFORM_STUB(AMediaCodec_stop)
PLATFORM_STUB(AMediaFormat_delete) PLATFORM_STUB(AMediaFormat_getBuffer)
PLATFORM_STUB(AMediaFormat_getInt32) PLATFORM_STUB(AMediaFormat_new)
PLATFORM_STUB(AMediaFormat_setBuffer) PLATFORM_STUB(AMediaFormat_setFloat)
PLATFORM_STUB(AMediaFormat_setInt32) PLATFORM_STUB(AMediaFormat_setString)
PLATFORM_STUB(AMediaFormat_toString) PLATFORM_STUB(ANativeWindow_acquire)
PLATFORM_STUB(ANativeWindow_fromSurface) PLATFORM_STUB(ANativeWindow_getHeight)
PLATFORM_STUB(ANativeWindow_getWidth) PLATFORM_STUB(ANativeWindow_release)
PLATFORM_STUB(__android_log_assert) PLATFORM_STUB(__android_log_buf_write)
PLATFORM_STUB(__android_log_print) PLATFORM_STUB(__android_log_write)
PLATFORM_STUB(android_set_abort_message)

/* OpenSL imports are interface IDs, i.e. exported data rather than calls. */
void* SL_IID_ANDROIDCONFIGURATION;
void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
void* SL_IID_BUFFERQUEUE;
void* SL_IID_ENGINE;
void* SL_IID_PLAY;
void* SL_IID_RECORD;
void* SL_IID_VOLUME;
