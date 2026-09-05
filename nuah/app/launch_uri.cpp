#include "nuah/launch_uri.hpp"
#include <stdexcept>
#include <string_view>
namespace nuah {
static std::string decode(std::string_view text) {
  std::string result;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      result += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    } else result += text[i] == '+' ? ' ' : text[i];
  }
  return result;
}
static void set(LaunchRequest& request, const std::string& key, const std::string& value) {
#define NUAH_SET(k_, m_) \
  if (key == k_) request.m_ = value; \
  else
  NUAH_SET("placeId", place_id) NUAH_SET("gameInstanceId", game_instance_id) NUAH_SET("reservedServerAccessCode", reserved_server_access_code) NUAH_SET("launchData", launch_data)
      NUAH_SET("callId", call_id) {
  }
#undef NUAH_SET
}
static std::string encode(std::string_view text) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char c : text) {
    const bool u = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (u) result += static_cast<char>(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 0x0f]; }
  }
  return result;
}
LaunchRequest parse_roblox_uri(const std::string& uri) {
  constexpr std::string_view prefixes[] = {"roblox://", "roblox-player://"};
  std::string_view query;
  bool matched = false;
  for (const auto prefix : prefixes)
    if (uri.rfind(prefix, 0) == 0) { query = std::string_view(uri).substr(prefix.size()); matched = true; break; }
  if (!matched) throw std::runtime_error("unsupported launch URI scheme");
  LaunchRequest request{};
  while (!query.empty()) {
    const auto next = query.find('&');
    const auto pair = query.substr(0, next);
    const auto equal = pair.find('=');
    if (equal != std::string_view::npos) set(request, decode(pair.substr(0, equal)), decode(pair.substr(equal + 1)));
    if (next == std::string_view::npos) break;
    query.remove_prefix(next + 1);
  }
  if (request.place_id.empty()) throw std::runtime_error("Roblox URI is missing placeId");
  return request;
}
std::string format_roblox_uri(const LaunchRequest& request) {
  if (request.place_id.empty()) { throw std::runtime_error("cannot format a Roblox URI without placeId"); }
  if (request.game_instance_id && request.reserved_server_access_code) { throw std::runtime_error("a Roblox URI cannot target both an instance and a reserved server"); }
  std::string uri = "roblox://placeId=" + encode(request.place_id);
  const char* keys[] = {"gameInstanceId", "reservedServerAccessCode", "launchData", "callId"};
  const std::optional<std::string>* values[] = {&request.game_instance_id, &request.reserved_server_access_code, &request.launch_data, &request.call_id};
  for (int i = 0; i < 4; ++i)
    if (*values[i]) { uri += '&'; uri += keys[i]; uri += '='; uri += encode(**values[i]); }
  return uri;
}
}  // namespace nuah
