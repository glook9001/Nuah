#include "nuah/native_window_bridge.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>

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

void* AAssetManager_fromJava(void*, void*) { return nullptr; }
void* AAssetManager_open(void*, const char*, int) { errno = ENOENT; return nullptr; }
void AAsset_close(void*) {}
const void* AAsset_getBuffer(void*) { return nullptr; }
long AAsset_getLength(void*) { return 0; }
int AAsset_openFileDescriptor(void*, long*, long*) { errno = ENOSYS; return -1; }

void* AConfiguration_new() { return new NuahConfiguration; }
void AConfiguration_delete(void* configuration) { delete static_cast<NuahConfiguration*>(configuration); }
void AConfiguration_fromAssetManager(void*, void*) {}
int AConfiguration_getCountry(void*) { return 0; }
int AConfiguration_getLanguage(void*) { return 0; }
int AConfiguration_getNavHidden(void*) { return 0; }
int AConfiguration_getScreenHeightDp(void*) { return 0; }
int AConfiguration_getScreenSize(void*) { return 0; }
int AConfiguration_getScreenWidthDp(void*) { return 0; }

void* ALooper_forThread() { static thread_local NuahLooper looper; return &looper; }
void* ALooper_prepare(int) { return ALooper_forThread(); }
void ALooper_acquire(void* looper) { if (looper) ++static_cast<NuahLooper*>(looper)->refs; }
void ALooper_release(void* looper) { if (looper && static_cast<NuahLooper*>(looper)->refs > 1) --static_cast<NuahLooper*>(looper)->refs; }
int ALooper_addFd(void*, int, int, int, void*, void*) { errno = ENOSYS; return -1; }
int ALooper_removeFd(void*, int) { return 0; }
int ALooper_pollOnce(int, int*, int*, void**) { return -1; }

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

void* AMediaCodec_createDecoderByType(const char*) { return nullptr; }
void* AMediaCodec_createEncoderByType(const char*) { return nullptr; }
int AMediaCodec_configure(void*, void*, void*, void*, int) { return -1; }
int AMediaCodec_start(void*) { return -1; }
int AMediaCodec_stop(void*) { return 0; }
int AMediaCodec_flush(void*) { return 0; }
void AMediaCodec_delete(void*) {}
ssize_t AMediaCodec_dequeueInputBuffer(void*, long) { return -1; }
ssize_t AMediaCodec_dequeueOutputBuffer(void*, void*, long) { return -1; }
uint8_t* AMediaCodec_getInputBuffer(void*, size_t, size_t*) { return nullptr; }
uint8_t* AMediaCodec_getOutputBuffer(void*, size_t, size_t*) { return nullptr; }
void* AMediaCodec_getOutputFormat(void*) { return nullptr; }
int AMediaCodec_queueInputBuffer(void*, size_t, size_t, size_t, long, uint32_t) { return -1; }
int AMediaCodec_releaseOutputBuffer(void*, size_t, bool) { return 0; }

void* AMediaFormat_new() { return nullptr; }
void AMediaFormat_delete(void*) {}
int AMediaFormat_getBuffer(void*, const char*, void**, size_t*) { return 0; }
int AMediaFormat_getInt32(void*, const char*, int32_t*) { return 0; }
void AMediaFormat_setBuffer(void*, const char*, const void*, size_t) {}
void AMediaFormat_setFloat(void*, const char*, float) {}
void AMediaFormat_setInt32(void*, const char*, int32_t) {}
void AMediaFormat_setString(void*, const char*, const char*) {}
const char* AMediaFormat_toString(void*) { return "NuahMediaFormat{}"; }

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
