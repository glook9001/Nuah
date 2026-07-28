#include "nuah/launch_uri.hpp"

#include <stdexcept>
#include <string_view>

namespace nuah {
namespace {
std::string decode(std::string_view text) {
  std::string result;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const auto hex = text.substr(i + 1, 2);
      const auto value = static_cast<char>(std::stoi(std::string(hex), nullptr, 16));
      result += value; i += 2;
    } else result += text[i] == '+' ? ' ' : text[i];
  }
  return result;
}
void set(LaunchRequest& request, const std::string& key, const std::string& value) {
  if (key == "placeId") request.place_id = value;
  else if (key == "gameInstanceId") request.game_instance_id = value;
  else if (key == "reservedServerAccessCode") request.reserved_server_access_code = value;
  else if (key == "launchData") request.launch_data = value;
  else if (key == "callId") request.call_id = value;
}

std::string encode(std::string_view text) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char character : text) {
    const bool unreserved =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' ||
        character == '_' || character == '.' || character == '~';
    if (unreserved) {
      result += static_cast<char>(character);
    } else {
      result += '%';
      result += hex[character >> 4];
      result += hex[character & 0x0f];
    }
  }
  return result;
}

void append(std::string& uri, const char* key, const std::string& value) {
  uri += '&';
  uri += key;
  uri += '=';
  uri += encode(value);
}
}

LaunchRequest parse_roblox_uri(const std::string& uri) {
  constexpr std::string_view prefix = "roblox://";
  constexpr std::string_view player_prefix = "roblox-player://";
  std::string_view query;
  if (uri.rfind(prefix, 0) == 0) query = std::string_view(uri).substr(prefix.size());
  else if (uri.rfind(player_prefix, 0) == 0) query = std::string_view(uri).substr(player_prefix.size());
  else throw std::runtime_error("unsupported launch URI scheme");
  LaunchRequest request{};
  while (!query.empty()) {
    const auto next = query.find('&'); const auto pair = query.substr(0, next);
    const auto equal = pair.find('=');
    if (equal != std::string_view::npos) set(request, decode(pair.substr(0, equal)), decode(pair.substr(equal + 1)));
    if (next == std::string_view::npos) break;
    query.remove_prefix(next + 1);
  }
  if (request.place_id.empty()) throw std::runtime_error("Roblox URI is missing placeId");
  return request;
}

std::string format_roblox_uri(const LaunchRequest& request) {
  if (request.place_id.empty()) {
    throw std::runtime_error("cannot format a Roblox URI without placeId");
  }
  if (request.game_instance_id && request.reserved_server_access_code) {
    throw std::runtime_error(
        "a Roblox URI cannot target both an instance and a reserved server");
  }
  std::string uri = "roblox://placeId=" + encode(request.place_id);
  if (request.game_instance_id) {
    append(uri, "gameInstanceId", *request.game_instance_id);
  }
  if (request.reserved_server_access_code) {
    append(uri, "reservedServerAccessCode",
           *request.reserved_server_access_code);
  }
  if (request.launch_data) append(uri, "launchData", *request.launch_data);
  if (request.call_id) append(uri, "callId", *request.call_id);
  return uri;
}
}  // namespace nuah
