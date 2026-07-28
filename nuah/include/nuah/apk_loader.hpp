#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace nuah {

struct ApkMember {
  std::string name;
  std::vector<std::byte> bytes;
};

class LoadedModule {
 public:
  LoadedModule() = default;
  ~LoadedModule();
  LoadedModule(const LoadedModule&) = delete;
  LoadedModule& operator=(const LoadedModule&) = delete;
  LoadedModule(LoadedModule&& other) noexcept;
  LoadedModule& operator=(LoadedModule&& other) noexcept;

  const std::filesystem::path& path() const { return path_; }
  void* handle() const { return handle_; }
  void* symbol(const char* name) const;
  std::size_t size() const { return size_; }

 private:
  friend LoadedModule load_apk_library(const std::filesystem::path&, const std::string&);
  std::filesystem::path path_;
  void* handle_ = nullptr;
  void* loader_library_ = nullptr;
  int (*close_)(void*) = nullptr;
  void* (*symbol_)(void*, const char*) = nullptr;
  std::size_t size_ = 0;
};

ApkMember read_stored_apk_member(const std::filesystem::path& apk, const std::string& member);
std::vector<ApkMember> read_apk_members_with_prefix(
    const std::filesystem::path& apk, const std::string& prefix);
std::vector<std::string> elf_needed_libraries(
    const std::vector<std::byte>& elf_bytes);
LoadedModule load_apk_library(const std::filesystem::path& apk,
                              const std::string& member);

}  // namespace nuah
