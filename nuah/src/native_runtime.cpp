#include "nuah/atl_backend.hpp"
#include "nuah/apk_loader.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace nuah {
namespace {

std::vector<std::filesystem::path> image_candidates(
    const NativeLaunchOptions& options) {
  std::vector<std::filesystem::path> result;
  result.reserve(options.split_apks.size() + 1);
  result.push_back(options.apk);
  for (const auto& split : options.split_apks) result.push_back(split);
  if (options.split_apks.empty()) {
    const auto sibling = options.apk.parent_path() / "split_config.x86_64.apk";
    if (std::filesystem::is_regular_file(sibling)) result.push_back(sibling);
  }
  return result;
}

std::filesystem::path find_image(const NativeLaunchOptions& options) {
  constexpr const char* kMember = "lib/x86_64/libroblox.so";
  for (const auto& apk : image_candidates(options)) {
    if (!std::filesystem::is_regular_file(apk)) continue;
    try {
      (void)read_stored_apk_member(apk, kMember);
      return apk;
    } catch (const std::exception&) {
      // The base APK often contains no native image; continue with the ABI
      // split instead of treating that normal layout as a fatal error.
    }
  }
  throw std::runtime_error(
      "selected APK set has no x86_64 lib/x86_64/libroblox.so");
}

std::filesystem::path runtime_directory() {
  std::array<char, 4096> path{};
  const auto size = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size <= 0 || static_cast<std::size_t>(size) >= path.size() - 1) {
    throw std::runtime_error("cannot locate Nuah runtime directory");
  }
  return std::filesystem::path(std::string(path.data(), size)).parent_path();
}

std::filesystem::path extract_roblox_image(const std::filesystem::path& apk) {
  const auto member = read_stored_apk_member(apk, "lib/x86_64/libroblox.so");
  char path[] = "/tmp/nuah-roblox-XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd < 0) throw std::runtime_error("cannot create temporary Roblox image");
  std::size_t offset = 0;
  while (offset < member.bytes.size()) {
    const auto written = ::write(fd, member.bytes.data() + offset,
                                 member.bytes.size() - offset);
    if (written <= 0) {
      ::close(fd);
      ::unlink(path);
      throw std::runtime_error("cannot write temporary Roblox image");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fchmod(fd, 0500) != 0 || ::close(fd) != 0) {
    ::unlink(path);
    throw std::runtime_error("cannot finalize temporary Roblox image");
  }
  return path;
}

int run_bionic_loader(const std::filesystem::path& image) {
  const auto root = runtime_directory();
  const auto linker = root / "bionic/lib64/linker64";
  const auto helper = root / "bionic/nuah-bionic-loader";
  // This namespace must contain Android ELF only.  In particular, do not add
  // Nuah's host-glibc android/ providers here: linker64 cannot load them.
  const auto library_path = (root / "bionic/lib64").string();
  if (!std::filesystem::is_regular_file(linker) ||
      !std::filesystem::is_regular_file(helper)) {
    throw std::runtime_error("Nuah bionic runtime bundle is missing linker64 or helper");
  }
  const auto child = ::fork();
  if (child < 0) throw std::runtime_error("cannot start bionic loader helper");
  if (child == 0) {
    // Android linker64's direct-exec mode takes an absolute program path as
    // argv[1]; configure its lookup path through the normal linker variable.
    ::setenv("LD_LIBRARY_PATH", library_path.c_str(), 1);
    ::execl(linker.c_str(), linker.c_str(), helper.c_str(), image.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child) {
    throw std::runtime_error("cannot wait for bionic loader helper");
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  if (WIFEXITED(status)) {
    throw std::runtime_error("bionic loader helper exited with status " +
                             std::to_string(WEXITSTATUS(status)));
  }
  throw std::runtime_error("bionic loader helper terminated by signal " +
                           std::to_string(WTERMSIG(status)));
}

}  // namespace

int run_native(const NativeLaunchOptions& options) {
  if (!std::filesystem::is_regular_file(options.apk)) {
    throw std::runtime_error("native APK does not exist: " +
                             options.apk.string());
  }
  if (options.width <= 0 || options.height <= 0) {
    throw std::runtime_error("native window dimensions must be positive");
  }

  const auto image_apk = find_image(options);
  constexpr const char* kMember = "lib/x86_64/libroblox.so";
  const auto image = extract_roblox_image(image_apk);
  try {
    run_bionic_loader(image);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(image, ignored);
    throw;
  }
  std::error_code ignored;
  std::filesystem::remove(image, ignored);
  std::cerr << "nuah native: API-36 bionic loader accepted " << kMember << " from "
            << image_apk << '\n';
  // Lifecycle/JNI moves into the bionic helper once the Android API provider
  // bridge is present.  This first slice proves the real-libc loader boundary.
  return 0;
}

}  // namespace nuah
