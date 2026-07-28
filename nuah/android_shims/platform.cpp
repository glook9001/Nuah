#include "nuah/native_window_bridge.h"
#include "nuah/android_abi_registry.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>

namespace {
void unsupported(const char* symbol) {
  nuah_android_api_unsupported("libandroid.so", symbol);
  errno = ENOSYS;
}
void unsupported_media(const char* symbol) {
  nuah_android_api_unsupported("libmediandk.so", symbol);
  errno = ENOSYS;
}
}

extern "C" {
struct NuahLooper { int refs = 1; };
struct NuahConfiguration { int unused = 0; };
extern const char AMEDIAFORMAT_KEY_MIME[] = "mime";
extern const char AMEDIAFORMAT_KEY_WIDTH[] = "width";
extern const char AMEDIAFORMAT_KEY_HEIGHT[] = "height";
extern const char AMEDIAFORMAT_KEY_COLOR_FORMAT[] = "color-format";
extern const char AMEDIAFORMAT_KEY_STRIDE[] = "stride";
extern const char AMEDIAFORMAT_KEY_BIT_RATE[] = "bitrate";
extern const char AMEDIAFORMAT_KEY_FRAME_RATE[] = "frame-rate";
extern const char AMEDIAFORMAT_KEY_I_FRAME_INTERVAL[] = "i-frame-interval";
extern const char AMEDIAFORMAT_KEY_CHANNEL_COUNT[] = "channel-count";
extern const char AMEDIAFORMAT_KEY_SAMPLE_RATE[] = "sample-rate";
static int sl_engine, sl_android_configuration, sl_buffer_queue, sl_volume, sl_play, sl_android_simple_buffer_queue, sl_record;
void* SL_IID_ENGINE = &sl_engine;
void* SL_IID_ANDROIDCONFIGURATION = &sl_android_configuration;
void* SL_IID_BUFFERQUEUE = &sl_buffer_queue;
void* SL_IID_VOLUME = &sl_volume;
void* SL_IID_PLAY = &sl_play;
void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &sl_android_simple_buffer_queue;
void* SL_IID_RECORD = &sl_record;

void* AAssetManager_fromJava(void*, void*) { unsupported("AAssetManager_fromJava"); return nullptr; }
void* AAssetManager_open(void*, const char*, int) { unsupported("AAssetManager_open"); return nullptr; }
void AAsset_close(void*) { unsupported("AAsset_close"); }
const void* AAsset_getBuffer(void*) { unsupported("AAsset_getBuffer"); return nullptr; }
long AAsset_getLength(void*) { unsupported("AAsset_getLength"); return -1; }
int AAsset_openFileDescriptor(void*, long*, long*) { unsupported("AAsset_openFileDescriptor"); return -1; }

void* AConfiguration_new() { return new NuahConfiguration; }
void AConfiguration_delete(void* configuration) { delete static_cast<NuahConfiguration*>(configuration); }
void AConfiguration_fromAssetManager(void*, void*) { unsupported("AConfiguration_fromAssetManager"); }
int AConfiguration_getCountry(void*) { unsupported("AConfiguration_getCountry"); return 0; }
int AConfiguration_getLanguage(void*) { unsupported("AConfiguration_getLanguage"); return 0; }
int AConfiguration_getNavHidden(void*) { unsupported("AConfiguration_getNavHidden"); return 0; }
int AConfiguration_getScreenHeightDp(void*) { return 720; }
int AConfiguration_getScreenSize(void*) { unsupported("AConfiguration_getScreenSize"); return 0; }
int AConfiguration_getScreenWidthDp(void*) { return 1280; }

void* ALooper_forThread() { static thread_local NuahLooper looper; return &looper; }
void* ALooper_prepare(int) { return ALooper_forThread(); }
void ALooper_acquire(void* looper) { if (looper) ++static_cast<NuahLooper*>(looper)->refs; }
void ALooper_release(void* looper) { if (looper && static_cast<NuahLooper*>(looper)->refs > 1) --static_cast<NuahLooper*>(looper)->refs; }
int ALooper_addFd(void*, int, int, int, void*, void*) { unsupported("ALooper_addFd"); return -1; }
int ALooper_removeFd(void*, int) { unsupported("ALooper_removeFd"); return -1; }
int ALooper_pollOnce(int, int*, int*, void**) { unsupported("ALooper_pollOnce"); return -1; }

void ANativeWindow_acquire(void* window) {
  nuah_native_window_acquire(static_cast<NuahNativeWindow*>(window));
}
void ANativeWindow_release(void* window) {
  nuah_native_window_release(static_cast<NuahNativeWindow*>(window));
}
void* ANativeWindow_fromSurface(void*, void* surface) {
  return nuah_native_window_from_surface(surface);
}
int ANativeWindow_getWidth(void* window) {
  return nuah_native_window_width(static_cast<NuahNativeWindow*>(window));
}
int ANativeWindow_getHeight(void* window) {
  return nuah_native_window_height(static_cast<NuahNativeWindow*>(window));
}

void* AMediaCodec_createDecoderByType(const char*) { unsupported_media("AMediaCodec_createDecoderByType"); return nullptr; }
void* AMediaCodec_createEncoderByType(const char*) { unsupported_media("AMediaCodec_createEncoderByType"); return nullptr; }
int AMediaCodec_configure(void*, void*, void*, void*, int) { unsupported_media("AMediaCodec_configure"); return -1; }
int AMediaCodec_start(void*) { unsupported_media("AMediaCodec_start"); return -1; }
int AMediaCodec_stop(void*) { unsupported_media("AMediaCodec_stop"); return -1; }
int AMediaCodec_flush(void*) { unsupported_media("AMediaCodec_flush"); return -1; }
void AMediaCodec_delete(void*) { unsupported_media("AMediaCodec_delete"); }
ssize_t AMediaCodec_dequeueInputBuffer(void*, long) { unsupported_media("AMediaCodec_dequeueInputBuffer"); return -1; }
ssize_t AMediaCodec_dequeueOutputBuffer(void*, void*, long) { unsupported_media("AMediaCodec_dequeueOutputBuffer"); return -1; }
uint8_t* AMediaCodec_getInputBuffer(void*, size_t, size_t*) { unsupported_media("AMediaCodec_getInputBuffer"); return nullptr; }
uint8_t* AMediaCodec_getOutputBuffer(void*, size_t, size_t*) { unsupported_media("AMediaCodec_getOutputBuffer"); return nullptr; }
void* AMediaCodec_getOutputFormat(void*) { unsupported_media("AMediaCodec_getOutputFormat"); return nullptr; }
int AMediaCodec_queueInputBuffer(void*, size_t, size_t, size_t, long, uint32_t) { unsupported_media("AMediaCodec_queueInputBuffer"); return -1; }
int AMediaCodec_releaseOutputBuffer(void*, size_t, bool) { unsupported_media("AMediaCodec_releaseOutputBuffer"); return -1; }

void* AMediaFormat_new() { unsupported_media("AMediaFormat_new"); return nullptr; }
void AMediaFormat_delete(void*) { unsupported_media("AMediaFormat_delete"); }
int AMediaFormat_getBuffer(void*, const char*, void**, size_t*) { unsupported_media("AMediaFormat_getBuffer"); return -1; }
int AMediaFormat_getInt32(void*, const char*, int32_t*) { unsupported_media("AMediaFormat_getInt32"); return -1; }
void AMediaFormat_setBuffer(void*, const char*, const void*, size_t) { unsupported_media("AMediaFormat_setBuffer"); }
void AMediaFormat_setFloat(void*, const char*, float) { unsupported_media("AMediaFormat_setFloat"); }
void AMediaFormat_setInt32(void*, const char*, int32_t) { unsupported_media("AMediaFormat_setInt32"); }
void AMediaFormat_setString(void*, const char*, const char*) { unsupported_media("AMediaFormat_setString"); }
const char* AMediaFormat_toString(void*) { unsupported_media("AMediaFormat_toString"); return nullptr; }

int slCreateEngine(void**, unsigned, const void*, unsigned, const void*, const bool*) { return 12; }
void ZSTD_trace_compress_begin(...) {}
void ZSTD_trace_compress_end(...) {}
void ZSTD_trace_decompress_begin(...) {}
void ZSTD_trace_decompress_end(...) {}
void __gcov_dump() {}
void __gcov_flush() {}
// Roblox registers this purchase callback through its Java bridge. Nuah's
// native-only Runtime has no purchase service, so the callback is a safe no-op.
void Java_com_roblox_client_purchase_IAPPurchaseManager_nativeFinishPaymentsProtocolPurchaseWithReturn(void*, void*, ...) {}
}  // extern "C"
