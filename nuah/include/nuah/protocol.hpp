#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace nuah {

enum class Opcode : std::uint16_t { command = 1, event = 2, error = 3 };

struct Frame {
  Opcode opcode;
  std::uint32_t request_id;
  std::string json;
};

bool send_frame(int fd, const Frame& frame, std::string& error);
bool receive_frame(int fd, Frame& frame, std::string& error);

// Sober Services' proven inherited-FD protocol.  It is intentionally fixed
// width: WebKit -> supervisor JSON is opcode 10 at byte 8; supervisor control
// records use their native opcode payload layout.
inline constexpr std::size_t kSoberRecordSize = 0x2830;
inline constexpr std::size_t kSoberMaximumWebKitJson = 0x2000;
inline constexpr std::uint8_t kSoberWebKitMessage = 10;
inline constexpr std::uint8_t kSoberLoadUri = 0;
inline constexpr std::uint8_t kSoberSetTitle = 2;
inline constexpr std::uint8_t kSoberSetVisible = 3;
inline constexpr std::uint8_t kSoberEvaluateJavaScript = 4;
inline constexpr std::uint8_t kSoberQuit = 5;

struct SoberRecord {
  std::uint8_t opcode = 0;
  std::string payload;
};

bool send_sober_control(int fd, std::uint8_t opcode,
                        const std::string& payload, std::string& error);
bool send_sober_webkit_json(int fd, const std::string& json,
                            std::string& error);
bool receive_sober_record(int fd, SoberRecord& record, std::string& error);

}  // namespace nuah
