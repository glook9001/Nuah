#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
const unsigned char kSlIidAndroidConfiguration = 0;
const unsigned char kSlIidAndroidSimpleBufferQueue = 0;
const unsigned char kSlIidBufferQueue = 0;
const unsigned char kSlIidEngine = 0;
const unsigned char kSlIidPlay = 0;
const unsigned char kSlIidRecord = 0;
const unsigned char kSlIidVolume = 0;
}  // namespace

extern "C" {

using AtlLooperWake = void (*)(void*);
void _ZN7android6Looper4wakeEv(void* looper) {
  if (std::getenv("NUAH_ATL_NOOP_LOOPER_WAKE")) return;
  static AtlLooperWake original = reinterpret_cast<AtlLooperWake>(
      ::dlsym(RTLD_NEXT, "_ZN7android6Looper4wakeEv"));
  if (original) original(looper);
}

/* ATL's Android Looper can allocate its wake event on descriptor 0 when the
 * launcher inherits a closed stdin.  Roblox later closes stdin as part of its
 * Android process setup, invalidating that event.  Keep descriptor 0 open only
 * for the explicit ATL diagnostic launch; normal Nuah processes retain the
 * host close semantics. */
int close(int fd) {
  if (fd == 0 && std::getenv("NUAH_ATL_KEEP_STDIN_FD")) return 0;
  return static_cast<int>(::syscall(SYS_close, fd));
}

// Android NDK MediaFormat exports these as pointer-valued data symbols.
const char* AMEDIAFORMAT_KEY_AAC_PROFILE = "aac-profile";
const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char* AMEDIAFORMAT_KEY_CHANNEL_MASK = "channel-mask";
const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char* AMEDIAFORMAT_KEY_DURATION = "durationUs";
const char* AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL =
    "flac-compression-level";
const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
const char* AMEDIAFORMAT_KEY_IS_ADTS = "is-adts";
const char* AMEDIAFORMAT_KEY_IS_AUTOSELECT = "is-autoselect";
const char* AMEDIAFORMAT_KEY_IS_DEFAULT = "is-default";
const char* AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE = "is-forced-subtitle";
const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char* AMEDIAFORMAT_KEY_LANGUAGE = "language";
const char* AMEDIAFORMAT_KEY_MAX_HEIGHT = "max-height";
const char* AMEDIAFORMAT_KEY_MAX_INPUT_SIZE = "max-input-size";
const char* AMEDIAFORMAT_KEY_MAX_WIDTH = "max-width";
const char* AMEDIAFORMAT_KEY_MIME = "mime";
const char* AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP =
    "push-blank-buffers-on-shutdown";
const char* AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER =
    "repeat-previous-frame-after";
const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char* AMEDIAFORMAT_KEY_WIDTH = "width";
const char* AMEDIAFORMAT_KEY_STRIDE = "stride";

// OpenSL ES interface IDs are opaque and compared by pointer identity.
const void* SL_IID_ANDROIDCONFIGURATION = &kSlIidAndroidConfiguration;
const void* SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &kSlIidAndroidSimpleBufferQueue;
const void* SL_IID_BUFFERQUEUE = &kSlIidBufferQueue;
const void* SL_IID_ENGINE = &kSlIidEngine;
const void* SL_IID_PLAY = &kSlIidPlay;
const void* SL_IID_RECORD = &kSlIidRecord;
const void* SL_IID_VOLUME = &kSlIidVolume;

std::int32_t AConfiguration_getScreenWidthDp(void*) { return 1280; }
std::int32_t AConfiguration_getScreenHeightDp(void*) { return 720; }

std::uint32_t slCreateEngine(void** engine, std::uint32_t,
                             const void*, std::uint32_t, const void*,
                             const unsigned char*) {
  if (engine) *engine = nullptr;
  // SL_RESULT_FEATURE_UNSUPPORTED. SDL3 audio will replace this compatibility
  // boundary without making native-library relocation depend on audio.
  return 12;
}

ssize_t bionic___sendto_chk(int socket_fd, const void* buffer,
                            std::size_t length, std::size_t buffer_size,
                            int flags, const struct sockaddr* destination,
                            socklen_t destination_length) {
  if (length > buffer_size ||
      length > static_cast<std::size_t>(
                   std::numeric_limits<ssize_t>::max())) {
    std::fputs("Nuah: __sendto_chk bounds violation\n", stderr);
    std::abort();
  }
  return ::sendto(socket_fd, buffer, length, flags, destination,
                  destination_length);
}

}  // extern "C"
