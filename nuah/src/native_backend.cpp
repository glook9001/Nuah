#include "nuah/native_backend.hpp"
#include <unistd.h>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "nuah/apk_loader.hpp"
namespace nuah {
namespace {
std::filesystem::path default_data_directory() {
  static constexpr struct { const char* env; const char* sub; } kDirs[] = {{"XDG_DATA_HOME", "nuah"}, {"HOME", ".local/share/nuah"}};
  for (const auto& d : kDirs)
    if (const char* v = std::getenv(d.env); v && *v) return std::filesystem::path(v) / d.sub;
  return std::filesystem::temp_directory_path() / ("nuah-" + std::to_string(::getuid()));
}
void write_member(const ApkMember& member, const std::filesystem::path& destination) {
  const auto temporary = destination.string() + ".nuah-new";
  { std::ofstream output(temporary, std::ios::binary | std::ios::trunc | std::ios::out);
    if (!output) throw std::runtime_error("cannot create extracted native library: " + temporary);
    output.write(reinterpret_cast<const char*>(member.bytes.data()), static_cast<std::streamsize>(member.bytes.size()));
    if (!output) throw std::runtime_error("cannot write extracted native library: " + temporary);
  }
  std::filesystem::rename(temporary, destination);
  std::error_code permission_error;
  std::filesystem::permissions(destination, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec | std::filesystem::perms::group_read | std::filesystem::perms::group_exec | std::filesystem::perms::others_read | std::filesystem::perms::others_exec, std::filesystem::perm_options::replace, permission_error);
  if (permission_error) throw std::runtime_error("cannot mark extracted native library executable: " + destination.string());
}
}  // namespace
std::filesystem::path prepare_app_native_libraries(const NativeLaunchOptions& options) {
  const auto data_directory = std::filesystem::absolute(options.data_directory.value_or(default_data_directory()));
  const auto library_directory = data_directory / (options.apk.filename().string() + "_") / "lib";
  std::filesystem::create_directories(library_directory);
  std::vector<std::filesystem::path> splits = options.split_apks;
  if (splits.empty())
    if (const auto sibling = options.apk.parent_path() / "split_config.x86_64.apk"; std::filesystem::is_regular_file(sibling)) splits.push_back(sibling);
  for (const auto& split : splits) {
    if (!std::filesystem::is_regular_file(split)) throw std::runtime_error("split APK does not exist: " + split.string());
    for (const auto& member : read_apk_members_with_prefix(split, "lib/x86_64/")) {
      const auto filename = std::filesystem::path(member.name).filename().string();
      if (!filename.empty() && filename.ends_with(".so")) write_member(member, library_directory / filename);
    }
  }
  return data_directory;
}
}  // namespace nuah
