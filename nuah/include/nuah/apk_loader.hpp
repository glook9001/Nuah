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
  void* (*versioned_symbol_)(void*, const char*, const char*) = nullptr;
  std::size_t size_ = 0;
  bool remove_path_ = true;
};

ApkMember read_stored_apk_member(const std::filesystem::path& apk, const std::string& member);
std::vector<ApkMember> read_apk_members_with_prefix(
    const std::filesystem::path& apk, const std::string& prefix);
std::vector<std::string> elf_needed_libraries(
    const std::vector<std::byte>& elf_bytes);

// Register an extracted app-library tree with ATL's bionic linker. This is
// the same dl_parse_library_path call made by ATL's executable before ART
// starts; setting an environment variable alone does not update its lookup
// table.
void configure_android_library_path(const std::filesystem::path& app_directory);

// Normalize Roblox's one native API-level import without loading a second
// libc_bio/TLS domain.  Must be called after ART has initialized the installed
// bionic linker and before app lifecycle callbacks begin.
bool patch_loaded_module_property_import(const LoadedModule& module);

// Apply the guarded TexturePackGenerator default directly to the mapped
// libroblox image.  This is deliberately opt-in at launch time and runs
// before ART can invoke JNI_OnLoad.
bool patch_loaded_module_texture_flag(const LoadedModule& module);

LoadedModule load_apk_library(const std::filesystem::path& apk,
                              const std::string& member);

}  // namespace nuah
