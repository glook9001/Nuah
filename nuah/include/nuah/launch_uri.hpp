#pragma once

#include <optional>
#include <string>

namespace nuah {

struct LaunchRequest {
  std::string place_id;
  std::optional<std::string> game_instance_id;
  std::optional<std::string> reserved_server_access_code;
  std::optional<std::string> launch_data;
  std::optional<std::string> call_id;
};

LaunchRequest parse_roblox_uri(const std::string& uri);

// Builds only the Android URI grammar recovered from Roblox's APK:
// placeId plus optional gameInstanceId, reservedServerAccessCode, and callId.
// It deliberately refuses mutually-exclusive instance/private-server targets.
std::string format_roblox_uri(const LaunchRequest& request);

}  // namespace nuah
