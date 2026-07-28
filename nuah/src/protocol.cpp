#include "nuah/protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace nuah {
namespace {
constexpr std::uint32_t kMagic = 0x4e554148;  // "NUAH"
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kMaximumPayload = 64 * 1024;
struct WireHeader { std::uint32_t magic; std::uint16_t version; std::uint16_t opcode; std::uint32_t request_id; std::uint32_t length; };
bool transfer(int fd, void* buffer, std::size_t size, bool write, std::string& error) {
  auto* bytes = static_cast<std::byte*>(buffer); std::size_t done = 0;
  while (done < size) {
    const auto n = write ? ::send(fd, bytes + done, size - done, MSG_NOSIGNAL) : ::recv(fd, bytes + done, size - done, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { error = n == 0 ? "peer disconnected" : "socket transfer failed"; return false; }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

std::size_t sober_payload_offset(std::uint8_t opcode) {
  // Rizin/live validation places JavaScript evaluation and helper JSON at
  // byte 8.  The simple native controls consume their string at byte 1.
  return opcode == kSoberEvaluateJavaScript || opcode == kSoberWebKitMessage
             ? 8
             : 1;
}

bool send_sober_raw(int fd, std::uint8_t opcode, const std::string& payload,
                    std::string& error) {
  const auto offset = sober_payload_offset(opcode);
  if (payload.size() >= kSoberRecordSize - offset) {
    error = "Sober Services record exceeds fixed payload capacity";
    return false;
  }
  std::array<std::byte, kSoberRecordSize> record{};
  record[0] = static_cast<std::byte>(opcode);
  std::memcpy(record.data() + offset, payload.data(), payload.size());
  return transfer(fd, record.data(), record.size(), true, error);
}
}
bool send_frame(int fd, const Frame& frame, std::string& error) {
  if (frame.json.size() > kMaximumPayload) { error = "frame exceeds maximum payload"; return false; }
  WireHeader header{kMagic, kVersion, static_cast<std::uint16_t>(frame.opcode), frame.request_id, static_cast<std::uint32_t>(frame.json.size())};
  return transfer(fd, &header, sizeof(header), true, error) &&
         (frame.json.empty() || transfer(fd, const_cast<char*>(frame.json.data()), frame.json.size(), true, error));
}
bool receive_frame(int fd, Frame& frame, std::string& error) {
  WireHeader header{};
  if (!transfer(fd, &header, sizeof(header), false, error)) return false;
  if (header.magic != kMagic || header.version != kVersion || header.length > kMaximumPayload) { error = "invalid Nuah frame"; return false; }
  frame.opcode = static_cast<Opcode>(header.opcode); frame.request_id = header.request_id; frame.json.assign(header.length, '\0');
  return frame.json.empty() || transfer(fd, frame.json.data(), frame.json.size(), false, error);
}

bool send_sober_control(int fd, std::uint8_t opcode,
                        const std::string& payload, std::string& error) {
  return send_sober_raw(fd, opcode, payload, error);
}

bool send_sober_webkit_json(int fd, const std::string& json,
                            std::string& error) {
  if (json.size() > kSoberMaximumWebKitJson) {
    error = "Sober Services rejects WebKit JSON above 0x2000 bytes";
    return false;
  }
  return send_sober_raw(fd, kSoberWebKitMessage, json, error);
}

bool receive_sober_record(int fd, SoberRecord& record, std::string& error) {
  std::array<std::byte, kSoberRecordSize> bytes{};
  if (!transfer(fd, bytes.data(), bytes.size(), false, error)) return false;
  record.opcode = static_cast<std::uint8_t>(bytes[0]);
  if (record.opcode == kSoberSetVisible) {
    record.payload.assign(
        1, static_cast<char>(bytes[1]));
    return true;
  }
  const auto offset = sober_payload_offset(record.opcode);
  const char* begin = reinterpret_cast<const char*>(bytes.data() + offset);
  const auto remaining = bytes.size() - offset;
  const auto length = ::strnlen(begin, remaining);
  record.payload.assign(begin, length);
  return true;
}
}  // namespace nuah
