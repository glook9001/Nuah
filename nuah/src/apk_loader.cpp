#include "nuah/apk_loader.hpp"
#include "nuah/bootstrap_diagnostics.h"

#include <elf.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <glib.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace nuah {
namespace {
constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kCentralDirectory = 0x02014b50;
constexpr std::uint32_t kLocalFile = 0x04034b50;

std::filesystem::path runtime_directory() {
  std::array<char, 4096> path{};
  const auto size = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size <= 0 || static_cast<std::size_t>(size) >= path.size() - 1) {
    throw std::runtime_error("cannot locate Nuah runtime directory");
  }
  return std::filesystem::path(std::string(path.data(), size)).parent_path();
}

void set_hybris_path_if_unset(const char* name, const std::string& value) {
  const char* existing = ::getenv(name);
  if (existing && *existing) return;
  if (::setenv(name, value.c_str(), 1) != 0) {
    throw std::runtime_error(std::string("cannot configure ") + name);
  }
}

// Only the tree beside the binary (./build/hybris) or an explicit override.
// Do not silently pick ~/.local/share or /usr/local; those can be stale.
std::filesystem::path hybris_common_library() {
  if (const char* configured = std::getenv("NUAH_HYBRIS_LIBRARY");
      configured && *configured) {
    return configured;
  }
  if (const char* configured = std::getenv("NUAH_HYBRIS_LIBRARY_DIR");
      configured && *configured) {
    return std::filesystem::path(configured) / "libhybris-common.so";
  }
  return runtime_directory() / "hybris" / "lib" / "libhybris-common.so";
}

void configure_hybris_environment(const char* library) {
  // Only dependency placeholders belong on the Android linker's search path.
  // The real Nuah providers are opened by the host loader below; putting them
  // on this path would make the Android linker chase their glibc dependencies.
  // The placeholders have no host dependencies and let upstream libhybris
  // reach its hook callback for every Android soname.
  const auto root = runtime_directory() / "android";
  const auto linker_dependencies = root / "linker-deps";
  const auto linker_path = std::filesystem::is_directory(linker_dependencies)
                               ? linker_dependencies
                               : root;  // compatibility with old artifacts
  set_hybris_path_if_unset("HYBRIS_LD_LIBRARY_PATH", linker_path.string());
  if (!library || !*library || ::getenv("HYBRIS_LINKER_DIR")) return;
  const std::filesystem::path common(library);
  if (!common.has_parent_path()) return;
  set_hybris_path_if_unset(
      "HYBRIS_LINKER_DIR",
      (common.parent_path() / "libhybris" / "linker").string());
}

std::vector<void*> host_provider_handles;
void* bionic_provider_handle = nullptr;
void* android_provider_handle = nullptr;
/* Optional hot-path synchronization provider. Nuah's finite bionic helper
 * keeps API-36 mutex objects in an out-of-line table, which is robust but
 * makes every lock/unlock take the table spin lock. The pinned
 * bionic-translation pthread wrapper stores the host pointer in the Android
 * object itself (with a low-bit tag), so it is a reversible A/B for Roblox's
 * FunctionMarshal-heavy rooms. */
void* pthread_bridge_handle = nullptr;
void* host_libc_handle = nullptr;
using HybrisBuiltinHook = void* (*)(const char*, const char*);
HybrisBuiltinHook hybris_builtin_hook = nullptr;
std::uintptr_t host_stack_chk_guard = 0x9e3779b97f4a7c15ULL;

// Keep the installed ATL/Bionic DSO (and its TLS domain) intact.  Its
// __system_property_get reports the API level baked into the distro bundle;
// replacing the whole libc_bio with a second build makes ART see duplicate
// Bionic state and crashes during startup.  The small app-local relocation
// below gives Roblox the same API-36 property as the Java façade.
extern "C" int nuah_api36_system_property_get(const char* key, char* value) {
  const char* result = "";
  if (key && std::strcmp(key, "ro.build.version.sdk") == 0) {
    result = "36";
  } else if (key && std::strcmp(key, "ro.build.version.release") == 0) {
    result = "10";
  } else if (key && std::strcmp(key, "ro.product.cpu.abi") == 0) {
    result = "x86_64";
  }
  if (value) std::strcpy(value, result);
  if (const char* trace = ::getenv("NUAH_PROPERTY_TRACE"); trace && *trace) {
    std::fprintf(stderr, "nuah property: %s -> %s\n", key ? key : "(null)",
                 result);
  }
  return static_cast<int>(std::strlen(result));
}

std::vector<std::byte> read_file(const std::filesystem::path& path);

bool patch_loaded_module_property_import_impl(const LoadedModule& module) {
#if !defined(__x86_64__)
  (void)module;
  return false;
#else
  /* Do not call bionic_dladdr here.  libhybris can expose a resolver
   * trampoline with a different Dl_info ABI; dereferencing its result was
   * the PRE_JNI_BIND crash.  The extracted ELF is available on disk, so use
   * its immutable symbol/relocation tables and derive the load bias from an
   * exported function address. */
  void* anchor = module.symbol(
      "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
  const char* anchor_name =
      "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode";
  if (!anchor) {
    anchor = module.symbol("JNI_OnLoad");
    anchor_name = "JNI_OnLoad";
  }
  if (!anchor || module.path().empty()) return false;

  const auto bytes = read_file(module.path());
  if (bytes.size() < sizeof(Elf64_Ehdr)) return false;
  Elf64_Ehdr header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_phentsize != sizeof(Elf64_Phdr) ||
      header.e_shentsize != sizeof(Elf64_Shdr)) {
    return false;
  }
  const auto in_bounds = [&](std::uint64_t offset, std::uint64_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
  };
  if (!in_bounds(header.e_phoff,
                 static_cast<std::uint64_t>(header.e_phnum) *
                     sizeof(Elf64_Phdr)) ||
      !in_bounds(header.e_shoff,
                 static_cast<std::uint64_t>(header.e_shnum) *
                     sizeof(Elf64_Shdr))) {
    return false;
  }
  const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(
      bytes.data() + header.e_phoff);
  const auto* shdrs = reinterpret_cast<const Elf64_Shdr*>(
      bytes.data() + header.e_shoff);

  const Elf64_Shdr* dynsym_section = nullptr;
  for (Elf64_Half index = 0; index < header.e_shnum; ++index) {
    const auto& section = shdrs[index];
    if (section.sh_type == SHT_DYNSYM && section.sh_entsize == sizeof(Elf64_Sym) &&
        in_bounds(section.sh_offset, section.sh_size) &&
        section.sh_link < header.e_shnum &&
        shdrs[section.sh_link].sh_type == SHT_STRTAB &&
        in_bounds(shdrs[section.sh_link].sh_offset,
                  shdrs[section.sh_link].sh_size)) {
      dynsym_section = &section;
      break;
    }
  }
  if (!dynsym_section) return false;
  const auto* symbols = reinterpret_cast<const Elf64_Sym*>(
      bytes.data() + dynsym_section->sh_offset);
  const std::size_t symbol_count =
      dynsym_section->sh_size / sizeof(Elf64_Sym);
  const auto& strtab_section = shdrs[dynsym_section->sh_link];
  const char* strings = reinterpret_cast<const char*>(
      bytes.data() + strtab_section.sh_offset);
  const std::size_t string_size = strtab_section.sh_size;

  std::uint64_t anchor_value = 0;
  for (std::size_t index = 0; index < symbol_count; ++index) {
    const auto& symbol = symbols[index];
    if (symbol.st_name >= string_size) continue;
    if (std::strcmp(strings + symbol.st_name, anchor_name) == 0) {
      anchor_value = symbol.st_value;
      break;
    }
  }
  if (anchor_value == 0) return false;
  const auto base = reinterpret_cast<std::uintptr_t>(anchor) - anchor_value;
  const auto page_size = static_cast<std::uintptr_t>(::getpagesize());
  const auto base_page = base & ~(page_size - 1);
  unsigned char resident = 0;
  if (::mincore(reinterpret_cast<void*>(base_page), page_size, &resident) != 0)
    return false;

  const Elf64_Phdr* dynamic_segment = nullptr;
  for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
    if (phdrs[index].p_type == PT_DYNAMIC) {
      dynamic_segment = &phdrs[index];
      break;
    }
  }
  if (!dynamic_segment || !in_bounds(dynamic_segment->p_offset,
                                     dynamic_segment->p_filesz)) {
    return false;
  }
  const Elf64_Dyn* dynamic = reinterpret_cast<const Elf64_Dyn*>(
      bytes.data() + dynamic_segment->p_offset);
  const std::size_t dynamic_count = dynamic_segment->p_filesz / sizeof(Elf64_Dyn);
  std::uint64_t jmprel_va = 0;
  std::size_t jmprel_size = 0;
  auto va_to_file = [&](std::uint64_t address, std::uint64_t size,
                        std::uint64_t* offset) {
    for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
      const auto& program = phdrs[index];
      if (program.p_type != PT_LOAD || address < program.p_vaddr) continue;
      const std::uint64_t delta = address - program.p_vaddr;
      if (delta <= program.p_filesz && size <= program.p_filesz - delta) {
        *offset = program.p_offset + delta;
        return in_bounds(*offset, size);
      }
    }
    return false;
  };
  for (std::size_t index = 0; index < dynamic_count; ++index) {
    if (dynamic[index].d_tag == DT_NULL) break;
    if (dynamic[index].d_tag == DT_JMPREL)
      jmprel_va = dynamic[index].d_un.d_ptr;
    else if (dynamic[index].d_tag == DT_PLTRELSZ)
      jmprel_size = dynamic[index].d_un.d_val;
  }
  if (!jmprel_va || jmprel_size == 0 ||
      jmprel_size % sizeof(Elf64_Rela) != 0)
    return false;
  std::uint64_t rel_offset = 0;
  if (!va_to_file(jmprel_va, jmprel_size, &rel_offset)) return false;
  const auto* relocations = reinterpret_cast<const Elf64_Rela*>(
      bytes.data() + rel_offset);
  const std::size_t relocation_count = jmprel_size / sizeof(Elf64_Rela);
  for (std::size_t index = 0; index < relocation_count; ++index) {
    const auto& relocation = relocations[index];
    if (ELF64_R_TYPE(relocation.r_info) != R_X86_64_JUMP_SLOT) continue;
    const auto symbol_index = ELF64_R_SYM(relocation.r_info);
    if (symbol_index >= symbol_count ||
        symbols[symbol_index].st_name >= string_size)
      continue;
    if (std::strcmp(strings + symbols[symbol_index].st_name,
                    "__system_property_get") != 0)
      continue;
    const auto slot_address = base + relocation.r_offset;
    const auto page = slot_address & ~(page_size - 1);
    if (::mincore(reinterpret_cast<void*>(page), page_size, &resident) != 0 ||
        ::mprotect(reinterpret_cast<void*>(page), page_size,
                   PROT_READ | PROT_WRITE) != 0)
      return false;
    *reinterpret_cast<void**>(slot_address) =
        reinterpret_cast<void*>(&nuah_api36_system_property_get);
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
      char value[93]{};
      const int length = nuah_api36_system_property_get(
          "ro.build.version.sdk", value);
      std::fprintf(stderr,
                   "nuah loader: patched libroblox property GOT=%p sdk=%d value=%s\n",
                   reinterpret_cast<void*>(slot_address), length, value);
    }
    return true;
  }
  return false;
#endif
}

struct TexturePatchSite {
  std::size_t file_offset = 0;
  std::uint64_t virtual_address = 0;
  int original_protection = PROT_READ | PROT_EXEC;
  bool already_patched = false;
};

std::optional<TexturePatchSite> find_texture_patch_site(
    const std::vector<std::byte>& bytes) {
#if !defined(__x86_64__)
  (void)bytes;
  return std::nullopt;
#else
  if (bytes.size() < sizeof(Elf64_Ehdr)) return std::nullopt;
  Elf64_Ehdr header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB ||
      header.e_machine != EM_X86_64 ||
      header.e_phentsize != sizeof(Elf64_Phdr)) {
    return std::nullopt;
  }
  const auto in_bounds = [&](std::uint64_t offset, std::uint64_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
  };
  if (!in_bounds(header.e_phoff,
                 static_cast<std::uint64_t>(header.e_phnum) *
                     sizeof(Elf64_Phdr))) {
    return std::nullopt;
  }

  std::vector<Elf64_Phdr> loads;
  loads.reserve(header.e_phnum);
  for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
    Elf64_Phdr program{};
    std::memcpy(&program,
                bytes.data() + header.e_phoff +
                    static_cast<std::size_t>(index) * sizeof(Elf64_Phdr),
                sizeof(program));
    if (program.p_type == PT_LOAD &&
        in_bounds(program.p_offset, program.p_filesz)) {
      loads.push_back(program);
    }
  }
  if (loads.empty()) return std::nullopt;

  const auto file_to_va = [&](std::size_t offset)
      -> std::optional<std::uint64_t> {
    for (const auto& program : loads) {
      if (offset >= program.p_offset &&
          static_cast<std::uint64_t>(offset) - program.p_offset <
              program.p_filesz) {
        return program.p_vaddr +
               (static_cast<std::uint64_t>(offset) - program.p_offset);
      }
    }
    return std::nullopt;
  };

  static constexpr char target_string[] =
      "TexturePackGeneratorUseOriginal";
  const std::size_t target_length = sizeof(target_string) - 1;
  std::vector<std::uint64_t> target_addresses;
  for (std::size_t offset = 0;
       offset + target_length <= bytes.size(); ++offset) {
    if (std::memcmp(bytes.data() + offset, target_string, target_length) != 0)
      continue;
    if (const auto address = file_to_va(offset))
      target_addresses.push_back(*address);
  }
  if (target_addresses.empty()) return std::nullopt;

  const auto has_target = [&](std::uint64_t address) {
    return std::find(target_addresses.begin(), target_addresses.end(), address) !=
           target_addresses.end();
  };
  static constexpr unsigned char prefix[] = {0x31, 0xc0, 0x88, 0x05};
  static constexpr unsigned char patched_prefix[] = {0xb0, 0x01, 0x88, 0x05};
  static constexpr unsigned char lea_prefix[] = {0x48, 0x8d, 0x0d};
  static constexpr unsigned char marker_prefix[] = {0x48, 0xc7, 0x05};
  std::vector<TexturePatchSite> matches;
  for (const auto& program : loads) {
    if ((program.p_flags & PF_X) == 0) continue;
    const std::size_t begin = static_cast<std::size_t>(program.p_offset);
    const std::size_t end = begin + static_cast<std::size_t>(program.p_filesz);
    // The candidate uses bytes through marker+10 (candidate+32).
    if (end < begin || end - begin < 33) continue;
    for (std::size_t candidate = begin; candidate + 33 <= end; ++candidate) {
      const bool original =
          std::memcmp(bytes.data() + candidate, prefix, sizeof(prefix)) == 0;
      const bool patched = std::memcmp(bytes.data() + candidate,
                                       patched_prefix,
                                       sizeof(patched_prefix)) == 0;
      if (!original && !patched) continue;
      const std::size_t lea = candidate + 8;
      if (std::memcmp(bytes.data() + lea, lea_prefix, sizeof(lea_prefix)) != 0)
        continue;
      std::int32_t displacement = 0;
      std::memcpy(&displacement, bytes.data() + lea + 3,
                  sizeof(displacement));
      const auto lea_address = file_to_va(lea);
      if (!lea_address)
        continue;
      const auto target_address = static_cast<std::int64_t>(*lea_address) +
                                  7 + static_cast<std::int64_t>(displacement);
      if (target_address < 0 ||
          !has_target(static_cast<std::uint64_t>(target_address)))
        continue;
      const std::size_t marker = candidate + 22;
      if (std::memcmp(bytes.data() + marker, marker_prefix,
                      sizeof(marker_prefix)) != 0 ||
          std::memcmp(bytes.data() + marker + 7, "\x1f\x00\x00\x00", 4) != 0)
        continue;
      const auto instruction_address = file_to_va(candidate);
      if (!instruction_address) continue;
      int protection = 0;
      if (program.p_flags & PF_R) protection |= PROT_READ;
      if (program.p_flags & PF_W) protection |= PROT_WRITE;
      if (program.p_flags & PF_X) protection |= PROT_EXEC;
      matches.push_back(TexturePatchSite{
          candidate, *instruction_address, protection, patched});
    }
  }
  if (matches.size() != 1) return std::nullopt;
  return matches.front();
#endif
}

std::optional<std::uintptr_t> module_load_bias_for_anchor(
    const LoadedModule& module, const std::vector<std::byte>& bytes,
    const char* anchor_name) {
#if !defined(__x86_64__)
  (void)module;
  (void)bytes;
  (void)anchor_name;
  return std::nullopt;
#else
  void* anchor = module.symbol(anchor_name);
  if (!anchor || bytes.size() < sizeof(Elf64_Ehdr)) return std::nullopt;
  Elf64_Ehdr header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (header.e_shentsize != sizeof(Elf64_Shdr)) return std::nullopt;
  const auto in_bounds = [&](std::uint64_t offset, std::uint64_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
  };
  if (!in_bounds(header.e_shoff,
                 static_cast<std::uint64_t>(header.e_shnum) *
                     sizeof(Elf64_Shdr))) {
    return std::nullopt;
  }
  std::vector<Elf64_Shdr> sections(header.e_shnum);
  std::memcpy(sections.data(), bytes.data() + header.e_shoff,
              sections.size() * sizeof(Elf64_Shdr));
  const Elf64_Shdr* dynsym = nullptr;
  for (const auto& section : sections) {
    if (section.sh_type == SHT_DYNSYM &&
        section.sh_entsize == sizeof(Elf64_Sym) &&
        in_bounds(section.sh_offset, section.sh_size) &&
        section.sh_link < sections.size() &&
        sections[section.sh_link].sh_type == SHT_STRTAB &&
        in_bounds(sections[section.sh_link].sh_offset,
                  sections[section.sh_link].sh_size)) {
      dynsym = &section;
      break;
    }
  }
  if (!dynsym) return std::nullopt;
  const auto& strings_section = sections[dynsym->sh_link];
  const auto* symbols = reinterpret_cast<const Elf64_Sym*>(
      bytes.data() + dynsym->sh_offset);
  const auto* strings = reinterpret_cast<const char*>(
      bytes.data() + strings_section.sh_offset);
  const std::size_t symbol_count = dynsym->sh_size / sizeof(Elf64_Sym);
  std::uint64_t symbol_value = 0;
  for (std::size_t index = 0; index < symbol_count; ++index) {
    const auto& symbol = symbols[index];
    if (symbol.st_name >= strings_section.sh_size ||
        symbol.st_shndx == SHN_UNDEF)
      continue;
    if (std::strcmp(strings + symbol.st_name, anchor_name) == 0) {
      symbol_value = symbol.st_value;
      break;
    }
  }
  if (symbol_value == 0) return std::nullopt;
  const auto address = reinterpret_cast<std::uintptr_t>(anchor);
  if (address < symbol_value) return std::nullopt;
  return address - symbol_value;
#endif
}

bool patch_loaded_module_texture_flag_impl(const LoadedModule& module) {
#if !defined(__x86_64__)
  (void)module;
  return false;
#else
  if (module.path().empty()) return false;
  const auto bytes = read_file(module.path());
  const auto site = find_texture_patch_site(bytes);
  if (!site) {
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
      std::fprintf(stderr,
                   "nuah loader: libroblox texture patch signature not found or ambiguous\n");
    return false;
  }

  std::optional<std::uintptr_t> load_bias;
  static constexpr const char* anchors[] = {
      "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode",
      "JNI_OnLoad"};
  for (const char* anchor : anchors) {
    load_bias = module_load_bias_for_anchor(module, bytes, anchor);
    if (load_bias) break;
  }
  if (!load_bias) return false;
  const auto target = *load_bias + site->virtual_address;
  const long page_size_value = ::sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) return false;
  const auto page_size = static_cast<std::uintptr_t>(page_size_value);
  const auto page = target & ~(page_size - 1);
  unsigned char resident = 0;
  if (::mincore(reinterpret_cast<void*>(page), page_size, &resident) != 0)
    return false;

  auto* instruction = reinterpret_cast<unsigned char*>(target);
  if (site->already_patched) {
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
      std::fprintf(stderr,
                   "nuah loader: libroblox texture default already enabled in memory at %p\n",
                   static_cast<void*>(instruction));
    return true;
  }
  if (instruction[0] != 0x31 || instruction[1] != 0xc0) return false;
  const int original_protection = site->original_protection
                                      ? site->original_protection
                                      : (PROT_READ | PROT_EXEC);
  if (::mprotect(reinterpret_cast<void*>(page), page_size,
                 original_protection | PROT_WRITE) != 0) {
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
      std::fprintf(stderr, "nuah loader: texture page mprotect(RWX) failed: %s\n",
                   std::strerror(errno));
    return false;
  }
  const unsigned char replacement[] = {0xb0, 0x01};
  std::memcpy(instruction, replacement, sizeof(replacement));
  __builtin___clear_cache(reinterpret_cast<char*>(instruction),
                          reinterpret_cast<char*>(instruction + sizeof(replacement)));
  const bool restored =
      ::mprotect(reinterpret_cast<void*>(page), page_size, original_protection) == 0;
  if (!restored) {
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
      std::fprintf(stderr, "nuah loader: texture page permission restore failed: %s\n",
                   std::strerror(errno));
    return false;
  }
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace)
    std::fprintf(stderr,
                 "nuah loader: patched libroblox texture default in memory at %p (file+0x%zx)\n",
                 static_cast<void*>(instruction), site->file_offset);
  return true;
#endif
}

// The production boundary is libhybris: it owns the Android linker-facing
// ABI and translates libc/pthread/TLS calls to this host.  Nuah's old bionic
// provider is retained only as an explicitly requested diagnostic fallback;
// preloading it creates a second TLS/DSO lifetime domain.
bool use_nuah_compat_runtime() {
  const char* mode = ::getenv("NUAH_ANDROID_RUNTIME");
  return mode && (std::strcmp(mode, "nuah") == 0 ||
                  std::strcmp(mode, "compat") == 0 ||
                  std::strcmp(mode, "diagnostic") == 0);
}

long android_sysconf(int name) {
  // bionic's sysconf names are ABI numbers, not portable source constants.
  // Forwarding the raw integer to glibc is wrong: for example Android 0x27
  // means page size while glibc interprets 39 as _SC_PHYS_PAGES.  Roblox's
  // allocator consumes these values during its ELF constructors.
  switch (name) {
    case 0x0006:
      return ::sysconf(_SC_CLK_TCK);
    case 0x000b:
      return ::sysconf(_SC_OPEN_MAX);
    case 0x0026:
      return ::sysconf(_SC_IOV_MAX);
    case 0x0027:
    case 0x0028:
      return ::sysconf(_SC_PAGESIZE);
    case 0x0060:
      return ::sysconf(_SC_NPROCESSORS_CONF);
    case 0x0061:
      return ::sysconf(_SC_NPROCESSORS_ONLN);
    case 0x0062:
      return ::sysconf(_SC_PHYS_PAGES);
    case 0x0063:
      return ::sysconf(_SC_AVPHYS_PAGES);
    default:
      errno = EINVAL;
      return -1;
  }
}

[[noreturn]] void android_fortify_fail(const char* check, std::size_t requested,
                                       std::size_t capacity) {
  // Do not quietly turn a Bionic bounds failure into SIGABRT.  Roblox starts
  // in a constructor, where this was otherwise indistinguishable from a
  // missing Android service.  A compatibility fallback must be deliberately
  // added per operation; blindly continuing here would permit an overflow.
  std::fprintf(stderr,
               "nuah bootstrap: Bionic fortify failure in %s "
               "(requested=%zu capacity=%zu; compatibility fallbacks: none)\n",
               check, requested, capacity);
  std::abort();
}

std::size_t android_strlen_chk(const char* text, std::size_t capacity) {
  if (!text) android_fortify_fail("__strlen_chk(null)", 0, capacity);
  const auto length = ::strnlen(text, capacity);
  // Match bionic's contract: a string that does not fit its advertised
  // object size is a fortify violation, never a silently truncated result.
  if (length == capacity) android_fortify_fail("__strlen_chk", length, capacity);
  return length;
}

void* android_memcpy_chk(void* destination, const void* source, std::size_t count,
                         std::size_t destination_capacity) {
  if (count > destination_capacity) {
    android_fortify_fail("__memcpy_chk", count, destination_capacity);
  }
  return std::memcpy(destination, source, count);
}

void* android_memmove_chk(void* destination, const void* source, std::size_t count,
                          std::size_t destination_capacity) {
  if (count > destination_capacity) {
    android_fortify_fail("__memmove_chk", count, destination_capacity);
  }
  return std::memmove(destination, source, count);
}

void* android_memset_chk(void* destination, int value, std::size_t count,
                         std::size_t destination_capacity) {
  if (count > destination_capacity) {
    android_fortify_fail("__memset_chk", count, destination_capacity);
  }
  return std::memset(destination, value, count);
}

char* android_strcpy_chk(char* destination, const char* source, std::size_t destination_capacity) {
  if (!source) android_fortify_fail("__strcpy_chk(null)", 0, destination_capacity);
  const auto length = ::strnlen(source, destination_capacity);
  if (length >= destination_capacity) {
    android_fortify_fail("__strcpy_chk", length + 1, destination_capacity);
  }
  return std::strcpy(destination, source);
}

char* android_strncpy_chk(char* destination, const char* source, std::size_t count,
                          std::size_t destination_capacity) {
  if (count > destination_capacity) {
    android_fortify_fail("__strncpy_chk", count, destination_capacity);
  }
  return std::strncpy(destination, source, count);
}

char* android_strncpy_chk2(char* destination, const char* source, std::size_t count,
                           std::size_t destination_capacity, std::size_t source_capacity) {
  (void)source_capacity;
  if (count > destination_capacity) {
    android_fortify_fail("__strncpy_chk2", count, destination_capacity);
  }
  return std::strncpy(destination, source, count);
}

char* android_strcat_chk(char* destination, const char* source, std::size_t destination_capacity) {
  const auto length = android_strlen_chk(destination, destination_capacity);
  const auto remaining = destination_capacity - length;
  if (android_strlen_chk(source, remaining) >= remaining) {
    android_fortify_fail("__strcat_chk", remaining, destination_capacity);
  }
  return std::strcat(destination, source);
}

char* android_strncat_chk(char* destination, const char* source, std::size_t count,
                           std::size_t destination_capacity) {
  const auto length = android_strlen_chk(destination, destination_capacity);
  const auto remaining = destination_capacity - length;
  if (count >= remaining && ::strnlen(source, count) == count) {
    android_fortify_fail("__strncat_chk", count, remaining);
  }
  return std::strncat(destination, source, count);
}

void* resolve_host_provider_symbol(const char* symbol, const char* requester) {
  if (!symbol) return nullptr;
  /* Roblox's native graphics gate reads this property directly through its
   * imported Bionic symbol.  Letting ATL's distro libc satisfy it first
   * exposes that bundle's older SDK (currently 16), even though ART and
   * GameActivity are configured for API 36.  Keep this as one narrow symbol
   * override; libc/pthread/TLS ownership remains with libhybris/ATL. */
  if (std::strcmp(symbol, "__system_property_get") == 0)
    return reinterpret_cast<void*>(&nuah_api36_system_property_get);
  /* The loader and JNI_OnLoad use native pthread_once_t storage.  Roblox's
   * post-init objects use the Android layout, so enable the out-of-line
   * adapter only after native_runtime has crossed the InitParams boundary. */
  const bool android_sync = [] {
    const char* value = ::getenv("NUAH_ANDROID_SYNC");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  const bool bridge_rwlock = android_sync && [] {
    const char* value = ::getenv("NUAH_PTHREAD_RWLOCK");
    return !value || !*value || std::strcmp(value, "0") != 0;
  }();
  const bool trace_provider = [] {
    const char* value = ::getenv("NUAH_TRACE_PROVIDER");
    return value && *value && std::strcmp(value, "0") != 0;
  }();
  const bool trace_pthread_symbol =
      std::strncmp(symbol, "pthread_", 8) == 0 ||
      std::strncmp(symbol, "__pthread_", 10) == 0;
  const auto traced_provider = [&](const char* owner, void* address) {
    if (trace_provider && trace_pthread_symbol) {
      std::fprintf(stderr,
                   "nuah provider: symbol=%s owner=%s address=%p requester=%s\n",
                   symbol, owner, address, requester ? requester : "(unknown)");
    }
    return address;
  };
  /* ATL's libandroid.so.0 is still required for its bionic_egl* exports,
   * but its ANativeWindow implementation is GTK-specific.  Roblox receives
   * Nuah's SDL-backed Surface, so let the Nuah provider own the complete
   * ANativeWindow family.  This must happen before RTLD_DEFAULT/ATL lookup;
   * otherwise ATL treats the Nuah object as a GtkWidget and dereferences it.
   */
  const bool native_window_symbol =
      std::strcmp(symbol, "ANativeWindow_fromSurface") == 0 ||
      std::strcmp(symbol, "ANativeWindow_acquire") == 0 ||
      std::strcmp(symbol, "ANativeWindow_release") == 0 ||
      std::strcmp(symbol, "ANativeWindow_getWidth") == 0 ||
      std::strcmp(symbol, "ANativeWindow_getHeight") == 0;
  if (native_window_symbol && android_provider_handle) {
    if (void* resolved = ::dlsym(android_provider_handle, symbol)) {
      return traced_provider("nuah-libandroid-window", resolved);
    }
  }
  /* libroblox also imports the NDK asset ABI directly.  The Nuah provider is
   * intentionally RTLD_LOCAL (its ANativeWindow layout must not interpose on
   * ATL's GTK implementation), so libhybris cannot discover these symbols
   * through RTLD_DEFAULT.  Resolve the complete AAsset/AAssetManager family
   * through the same provider-owned handle. */
  const bool asset_symbol = std::strncmp(symbol, "AAsset", 6) == 0;
  if (asset_symbol && android_provider_handle) {
    if (void* resolved = ::dlsym(android_provider_handle, symbol)) {
      return traced_provider("nuah-libandroid-assets", resolved);
    }
  }
  /* Native Roblox links libEGL.so directly.  Nuah's Android-facing provider
   * owns the window façade, so route its bionic_ entry points before falling
   * back to host Mesa. */
  if (android_provider_handle &&
      (std::strncmp(symbol, "egl", 3) == 0 ||
       std::strncmp(symbol, "gl", 2) == 0)) {
    const std::string bionic_name = std::string("bionic_") + symbol;
    if (void* resolved = ::dlsym(android_provider_handle, bionic_name.c_str())) {
      return resolved;
    }
  }
  // A small audited set must precede libhybris: these objects have different
  // API-36 x86-64 layouts (pthread attributes and legacy Bionic FILE), or
  // carry Nuah's constructor diagnostics. Everything else delegates to
  // libhybris before the finite provider fallback.
  const bool compat_pthread_provider = use_nuah_compat_runtime();
  const bool requires_bionic_sync =
      android_sync &&
      (std::strcmp(symbol, "pthread_mutex_init") == 0 ||
       std::strcmp(symbol, "pthread_mutex_destroy") == 0 ||
       std::strcmp(symbol, "pthread_mutex_lock") == 0 ||
       std::strcmp(symbol, "pthread_mutex_trylock") == 0 ||
       std::strcmp(symbol, "pthread_mutex_unlock") == 0 ||
       std::strcmp(symbol, "pthread_mutexattr_init") == 0 ||
       std::strcmp(symbol, "pthread_mutexattr_destroy") == 0 ||
       std::strcmp(symbol, "pthread_mutexattr_settype") == 0 ||
       std::strcmp(symbol, "pthread_cond_init") == 0 ||
       std::strcmp(symbol, "pthread_cond_destroy") == 0 ||
       std::strcmp(symbol, "pthread_cond_signal") == 0 ||
       std::strcmp(symbol, "pthread_cond_broadcast") == 0 ||
       std::strcmp(symbol, "pthread_cond_wait") == 0 ||
       std::strcmp(symbol, "pthread_cond_timedwait") == 0 ||
       std::strcmp(symbol, "pthread_condattr_init") == 0 ||
       std::strcmp(symbol, "pthread_condattr_destroy") == 0 ||
       std::strcmp(symbol, "pthread_condattr_setclock") == 0 ||
       (bridge_rwlock &&
        (std::strcmp(symbol, "pthread_rwlock_init") == 0 ||
         std::strcmp(symbol, "pthread_rwlock_destroy") == 0 ||
         std::strcmp(symbol, "pthread_rwlock_rdlock") == 0 ||
         std::strcmp(symbol, "pthread_rwlock_unlock") == 0 ||
         std::strcmp(symbol, "pthread_rwlock_wrlock") == 0)));
  const bool requires_bionic_pthread =
      requires_bionic_sync ||
      (compat_pthread_provider &&
       (std::strcmp(symbol, "pthread_attr_init") == 0 ||
       std::strcmp(symbol, "pthread_attr_destroy") == 0 ||
       std::strcmp(symbol, "pthread_attr_getstack") == 0 ||
       std::strcmp(symbol, "pthread_attr_setdetachstate") == 0 ||
       std::strcmp(symbol, "pthread_attr_setschedparam") == 0 ||
       std::strcmp(symbol, "pthread_attr_setstacksize") == 0 ||
       std::strcmp(symbol, "pthread_create") == 0 ||
       std::strcmp(symbol, "pthread_getattr_np") == 0 ||
       (std::strcmp(symbol, "pthread_once") == 0 && android_sync)));
  if (requires_bionic_sync && pthread_bridge_handle) {
    /* The standalone bridge exports the Android ABI under bionic_ names so it
     * can coexist with glibc. Mutex/condition calls are the default handoff;
     * rwlocks are opt-in because their Android object layout must be proven
     * against the exact client before changing the production path. Attributes,
     * thread creation, and TLS remain on the existing provider. */
    const bool bridge_symbol =
        std::strncmp(symbol, "pthread_mutex", 13) == 0 ||
        std::strncmp(symbol, "pthread_cond", 12) == 0 ||
        std::strncmp(symbol, "pthread_condattr", 15) == 0 ||
        (bridge_rwlock && std::strncmp(symbol, "pthread_rwlock", 14) == 0);
    if (bridge_symbol) {
      const std::string bridged_name = std::string("bionic_") + symbol;
      if (void* resolved = ::dlsym(pthread_bridge_handle,
                                   bridged_name.c_str())) {
        return traced_provider("pthread-bio", resolved);
      }
      if (trace_provider) {
        std::fprintf(stderr,
                     "nuah provider: pthread bridge missing %s for %s\n",
                     bridged_name.c_str(), symbol);
      }
    }
  }
  const bool requires_bionic_provider =
      std::strcmp(symbol, "abort") == 0 ||
      std::strcmp(symbol, "fflush") == 0 ||
      std::strcmp(symbol, "fread") == 0 ||
      std::strcmp(symbol, "fwrite") == 0 ||
      std::strcmp(symbol, "fclose") == 0 ||
      std::strcmp(symbol, "__sF") == 0 ||
      std::strcmp(symbol, "stdin") == 0 ||
      std::strcmp(symbol, "stdout") == 0 ||
      std::strcmp(symbol, "stderr") == 0 ||
      requires_bionic_pthread;
  /* These are the small Android-only ABI helpers that glibc/libhybris do not
   * expose.  In production the bionic shim is loaded LOCAL solely for this
   * finite set; libhybris still owns all libc/pthread/TLS/DSO resolution. */
  const bool requires_bionic_helper =
      std::strcmp(symbol, "__FD_CLR_chk") == 0 ||
      std::strcmp(symbol, "__FD_ISSET_chk") == 0 ||
      std::strcmp(symbol, "__FD_SET_chk") == 0 ||
      std::strcmp(symbol, "__assert2") == 0 ||
      std::strcmp(symbol, "__fwrite_chk") == 0 ||
      std::strcmp(symbol, "__sendto_chk") == 0 ||
      std::strcmp(symbol, "__stack_chk_guard") == 0 ||
      std::strcmp(symbol, "__strchr_chk") == 0 ||
      std::strcmp(symbol, "__strlen_chk") == 0 ||
      std::strcmp(symbol, "__strncpy_chk2") == 0 ||
      std::strcmp(symbol, "__system_property_get") == 0 ||
      std::strcmp(symbol, "__write_chk") == 0 ||
      std::strcmp(symbol, "android_set_abort_message") == 0;
  if (bionic_provider_handle && requires_bionic_helper) {
    // The standalone Nuah helper DSO is intentionally unversioned, while
    // libc_bio uses the Android LIBC version node.  Accept either shape;
    // requiring only LIBC made real fortify imports such as __sendto_chk
    // fail even though the provider exported the correct ABI.
    if (void* resolved = ::dlvsym(bionic_provider_handle, symbol, "LIBC")) {
      return resolved;
    }
    if (void* resolved = ::dlsym(bionic_provider_handle, symbol)) {
      return resolved;
    }
  }
  if (bionic_provider_handle && requires_bionic_provider) {
    if (void* resolved =
            ::dlvsym(bionic_provider_handle, symbol, "LIBC")) {
      return resolved;
    }
  }
  if (use_nuah_compat_runtime()) {
    if (std::strcmp(symbol, "__stack_chk_guard") == 0) return &host_stack_chk_guard;
    if (std::strcmp(symbol, "__stack_chk_fail") == 0) {
      return ::dlsym(RTLD_DEFAULT, symbol);
    }
    if (std::strcmp(symbol, "__strlen_chk") == 0) {
      return reinterpret_cast<void*>(android_strlen_chk);
    }
    if (std::strcmp(symbol, "__memcpy_chk") == 0) return reinterpret_cast<void*>(android_memcpy_chk);
    if (std::strcmp(symbol, "__memmove_chk") == 0) return reinterpret_cast<void*>(android_memmove_chk);
    if (std::strcmp(symbol, "__memset_chk") == 0) return reinterpret_cast<void*>(android_memset_chk);
    if (std::strcmp(symbol, "__strcpy_chk") == 0) return reinterpret_cast<void*>(android_strcpy_chk);
    if (std::strcmp(symbol, "__strncpy_chk") == 0) return reinterpret_cast<void*>(android_strncpy_chk);
    if (std::strcmp(symbol, "__strncpy_chk2") == 0) return reinterpret_cast<void*>(android_strncpy_chk2);
    if (std::strcmp(symbol, "__strcat_chk") == 0) return reinterpret_cast<void*>(android_strcat_chk);
    if (std::strcmp(symbol, "__strncat_chk") == 0) return reinterpret_cast<void*>(android_strncat_chk);
  }
  /* Android's request numbers differ from glibc's.  Keep this one narrow
   * translation in the libhybris hook; it does not provide another libc. */
  if (std::strcmp(symbol, "sysconf") == 0) {
    return reinterpret_cast<void*>(android_sysconf);
  }
  const bool is_pthread_symbol =
      std::strncmp(symbol, "pthread_", 8) == 0 ||
      std::strncmp(symbol, "__pthread_", 10) == 0;
  /* The explicit compatibility provider remains available for diagnostics.
   * In production, Roblox's API-36 pthread objects are forwarded to the
   * process's one glibc TLS domain; libhybris still owns Android DSO loading. */
  if (use_nuah_compat_runtime() && bionic_provider_handle &&
      is_pthread_symbol) {
    if (void* resolved = ::dlvsym(bionic_provider_handle, symbol, "LIBC")) {
      return traced_provider("nuah-bionic", resolved);
    }
  }
  if (!use_nuah_compat_runtime() && is_pthread_symbol) {
    /* Do not use RTLD_DEFAULT here.  The Android helper DSO is deliberately
     * kept local, but a default-scope lookup can still select its exported
     * pthread trampolines on glibc.  That creates a second pthread state
     * machine and, during Roblox's TLS singleton initialization, recurses
     * back into the caller.  Pin production pthread symbols to the process's
     * real libc instead. */
    if (host_libc_handle) {
      if (void* resolved = ::dlsym(host_libc_handle, symbol)) {
        return traced_provider("host-libc", resolved);
      }
    }
    if (void* resolved = ::dlsym(RTLD_NEXT, symbol)) {
      return traced_provider("rtld-next", resolved);
    }
  }
  for (auto it = host_provider_handles.rbegin(); it != host_provider_handles.rend(); ++it) {
    void* resolved = ::dlsym(*it, symbol);
    if (!resolved) continue;
    Dl_info owner{};
    link_map* provider = nullptr;
    if (::dladdr(resolved, &owner) != 0 &&
        ::dlinfo(*it, RTLD_DI_LINKMAP, &provider) == 0 && provider &&
        owner.dli_fbase == reinterpret_cast<void*>(provider->l_addr)) {
      return traced_provider("host-provider", resolved);
    }
  }
  if (hybris_builtin_hook) {
    if (void* resolved = hybris_builtin_hook(symbol, requester)) {
      return traced_provider("libhybris-builtin", resolved);
    }
  }
  /* The Android image contains ordinary libc/libm/libpthread imports in
   * addition to libhybris's special Android hooks.  They are intentionally
   * satisfied by the one host libc domain in production.  Keeping this
   * fallback here also makes the preflight reflect the same rule used by the
   * linker instead of rejecting the image for every ordinary glibc symbol. */
  if (!use_nuah_compat_runtime()) {
    if (void* resolved = ::dlsym(RTLD_DEFAULT, symbol)) {
      return traced_provider("rtld-default", resolved);
    }
  }
  // libhybris may still satisfy this through one of its built-in host hooks,
  // so keep this trace opt-in and never treat a callback miss as a loader
  // failure by itself.
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::fprintf(stderr, "nuah bootstrap: provider miss symbol=%s requester=%s\n",
                 symbol, requester ? requester : "(unknown)");
  }
  return nullptr;
}

void load_host_provider(const std::filesystem::path& path) {
  /* Nuah's libandroid.so and ATL's libandroid.so.0 intentionally expose the
   * same NDK symbol names but different ANativeWindow layouts (SDL façade vs
   * GTK SurfaceView).  Keep Nuah's provider local: libhybris still resolves
   * app imports through its explicit handle, while ATL's own JNI library can
   * bind its GTK ANativeWindow symbols to libandroid.so.0 without accidental
   * ELF interposition. */
  const int flags = path.filename() == "libandroid.so"
                        ? (RTLD_NOW | RTLD_LOCAL)
                        : (RTLD_NOW | RTLD_GLOBAL);
  void* handle = ::dlopen(path.c_str(), flags);
  if (handle) {
    if (path.filename() == "libbionic.so") bionic_provider_handle = handle;
    if (path.filename() == "libandroid.so") android_provider_handle = handle;
    const auto callbacks = nuah_bootstrap_diagnostics_callbacks();
    if (auto setter = reinterpret_cast<void (*)(
            const NuahDiagnosticsCallbacks*)>(
            ::dlsym(handle, "nuah_bionic_set_diagnostics_callbacks"))) {
      setter(&callbacks);
    }
    if (auto setter = reinterpret_cast<void (*)(
            const NuahDiagnosticsCallbacks*)>(
            ::dlsym(handle, "nuah_log_set_diagnostics_callbacks"))) {
      setter(&callbacks);
    }
    host_provider_handles.push_back(handle);
    return;
  }
  const char* error = ::dlerror();
  throw std::runtime_error("cannot load Nuah host provider " + path.string() + ": " +
                           (error ? error : "unknown error"));
}

void configure_host_provider_hooks(void* hybris) {
  static bool configured = false;
  if (configured) return;
  const auto android = runtime_directory() / "android";
  // Never load Nuah's libbionic in the normal path.  libhybris resolves the
  // Android libc/libdl/libm/pthread ABI through its host hooks, keeping one
  // native TLS domain.  Set NUAH_ANDROID_RUNTIME=nuah only for the old
  // diagnostic provider path.
  const std::vector<const char*> providers =
      use_nuah_compat_runtime()
          ? std::vector<const char*>{"libbionic.so", "libm.so", "liblog.so",
                                    "libandroid.so", "libvulkan.so",
                                    "libmediandk.so", "libOpenSLES.so",
                                    "libOpenMAXAL.so"}
          : std::vector<const char*>{"liblog.so", "libandroid.so",
                                    "libvulkan.so", "libmediandk.so",
                                    "libOpenSLES.so", "libOpenMAXAL.so"};
  for (const auto* name : providers) {
    load_host_provider(android / name);
  }
  if (!use_nuah_compat_runtime()) {
    /* The Android linker resolves a small set of legacy helper imports
     * through RTLD_DEFAULT (not through libhybris's callback).  Keep this
     * helper DSO visible so imports such as __sendto_chk can be resolved
     * before the app image is entered.  It does not replace libc: ordinary
     * libc/pthread/TLS lookups still resolve to the host/libhybris domain
     * through resolve_host_provider_symbol(). */
    const auto linker_helpers = android / "libbionic-linker-helpers.so";
    void* helper_handle = ::dlopen(linker_helpers.c_str(),
                                   RTLD_NOW | RTLD_GLOBAL);
    if (!helper_handle) {
      const char* error = ::dlerror();
      throw std::runtime_error(
          "cannot load Android linker helper provider: " +
          std::string(error ? error : "unknown error"));
    }
    host_provider_handles.push_back(helper_handle);
    bionic_provider_handle = ::dlopen((android / "libbionic.so").c_str(),
                                      RTLD_NOW | RTLD_LOCAL);
    if (!bionic_provider_handle) {
      const char* error = ::dlerror();
      throw std::runtime_error(
          "cannot load Nuah Android helper provider: " +
          std::string(error ? error : "unknown error"));
    }
    const auto callbacks = nuah_bootstrap_diagnostics_callbacks();
    if (auto setter = reinterpret_cast<void (*)(
            const NuahDiagnosticsCallbacks*)>(
            ::dlsym(bionic_provider_handle,
                    "nuah_bionic_set_diagnostics_callbacks"))) {
      setter(&callbacks);
    }
  }
  /* Prefer the pointer-tagged bridge when the local binary exists. It removes
   * the table spin lock from the hot mutex/condition path. Set
   * NUAH_PTHREAD_SYNC=table for the old provider or use bridge explicitly on
   * an installation that supplies the binary separately. A missing optional
   * binary always keeps the known-good table adapter. */
  const char* pthread_sync_mode = ::getenv("NUAH_PTHREAD_SYNC");
  const bool prefer_pthread_bridge =
      !pthread_sync_mode || std::strcmp(pthread_sync_mode, "bridge") == 0;
  if (prefer_pthread_bridge) {
    std::vector<std::filesystem::path> candidates;
    if (const char* configured = ::getenv("NUAH_PTHREAD_BRIDGE_LIBRARY");
        configured && *configured) {
      candidates.emplace_back(configured);
    }
    candidates.emplace_back(runtime_directory() / "bionic-translation" /
                             "libpthread_bio.so.0");
    for (const auto& candidate : candidates) {
      if (!std::filesystem::is_regular_file(candidate)) continue;
      pthread_bridge_handle =
          ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (pthread_bridge_handle) {
        if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
            trace && *trace) {
          std::fprintf(stderr, "nuah sync: pthread bridge=%s\n",
                       candidate.c_str());
        }
        break;
      }
    }
    if (!pthread_bridge_handle && ::getenv("NUAH_BOOTSTRAP_TRACE")) {
      std::fprintf(stderr,
                   "nuah sync: requested pthread bridge unavailable; using table adapter\n");
    }
  }
  host_libc_handle = ::dlopen("libc.so.6", RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
  if (!host_libc_handle) {
    const char* error = ::dlerror();
    throw std::runtime_error(
        "cannot pin host libc for Android pthread symbols: " +
        std::string(error ? error : "libc.so.6 is not loaded"));
  }
  for (const auto* name : {"libEGL.so.1", "libGLESv2.so.2"}) {
    void* handle = ::dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
      const char* error = ::dlerror();
      throw std::runtime_error(std::string("cannot load host graphics provider ") + name + ": " +
                               (error ? error : "unknown error"));
    }
    host_provider_handles.push_back(handle);
  }
  const auto set_hook_callback = reinterpret_cast<void (*)(void* (*)(const char*, const char*))>(
      ::dlsym(hybris, "hybris_set_hook_callback"));
  if (!set_hook_callback) throw std::runtime_error("libhybris lacks hook callback support");
  hybris_builtin_hook = reinterpret_cast<HybrisBuiltinHook>(
      ::dlsym(hybris, "hybris_get_builtin_hook"));
  if (!hybris_builtin_hook) {
    throw std::runtime_error(
        "libhybris bundle lacks the Nuah built-in-first hook API");
  }
  set_hook_callback(resolve_host_provider_symbol);
  configured = true;
}

struct MappedFile {
  void* data = nullptr;
  std::size_t size = 0;
  int fd = -1;

  MappedFile() = default;
  explicit MappedFile(const std::filesystem::path& path) {
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      throw std::runtime_error("cannot open APK: " + path.string());
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      throw std::runtime_error("cannot stat APK: " + path.string());
    }
    size = static_cast<std::size_t>(st.st_size);
    if (size == 0) return;
    data = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
      ::close(fd);
      throw std::runtime_error("cannot mmap APK: " + path.string());
    }
    ::madvise(data, size, MADV_WILLNEED | MADV_SEQUENTIAL);
  }

  ~MappedFile() {
    if (data && data != MAP_FAILED && size > 0) ::munmap(data, size);
    if (fd >= 0) ::close(fd);
  }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept : data(other.data), size(other.size), fd(other.fd) {
    other.data = nullptr;
    other.size = 0;
    other.fd = -1;
  }
  MappedFile& operator=(MappedFile&& other) noexcept {
    if (this != &other) {
      if (data && data != MAP_FAILED && size > 0) ::munmap(data, size);
      if (fd >= 0) ::close(fd);
      data = other.data;
      size = other.size;
      fd = other.fd;
      other.data = nullptr;
      other.size = 0;
      other.fd = -1;
    }
    return *this;
  }

  std::span<const std::byte> span() const noexcept {
    if (!data || data == MAP_FAILED) return {};
    return {reinterpret_cast<const std::byte*>(data), size};
  }
};

inline std::uint16_t u16(std::span<const std::byte> b, std::size_t off) {
  if (off + 2 > b.size()) throw std::runtime_error("truncated ZIP field");
  return static_cast<std::uint16_t>(static_cast<unsigned char>(b[off])) |
         (static_cast<std::uint16_t>(static_cast<unsigned char>(b[off + 1])) << 8);
}
inline std::uint32_t u32(std::span<const std::byte> b, std::size_t off) {
  if (off + 4 > b.size()) throw std::runtime_error("truncated ZIP field");
  return static_cast<std::uint32_t>(static_cast<unsigned char>(b[off])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 3])) << 24);
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("cannot open APK: " + path.string());
  const auto n = in.tellg();
  if (n < 0) throw std::runtime_error("cannot determine APK size");
  std::vector<std::byte> data(static_cast<std::size_t>(n));
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(data.data()), n)) throw std::runtime_error("cannot read APK");
  return data;
}

void validate_elf(const std::vector<std::byte>& data) {
  if (data.size() < sizeof(Elf64_Ehdr)) throw std::runtime_error("APK member is too small for ELF64");
  Elf64_Ehdr header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64 || header.e_type != ET_DYN) {
    throw std::runtime_error("APK member is not an x86-64 PIE/shared ELF");
  }
}

std::string sha256_hex(const std::vector<std::byte>& data) {
  GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
  if (!checksum) throw std::runtime_error("cannot create SHA-256 checksum");
  g_checksum_update(
      checksum, reinterpret_cast<const guchar*>(data.data()), data.size());
  const char* value = g_checksum_get_string(checksum);
  const std::string result = value ? value : "";
  g_checksum_free(checksum);
  if (result.empty()) throw std::runtime_error("cannot calculate SHA-256 checksum");
  return result;
}

std::optional<std::string> patch_manifest_field(const std::string& manifest,
                                                const char* field) {
  const std::regex pattern(
      "\\\"" + std::string(field) +
      "\\\"\\s*:\\s*(?:\\\"([^\\\"]*)\\\"|([0-9]+))");
  std::smatch match;
  if (!std::regex_search(manifest, match, pattern) || match.size() != 3)
    return std::nullopt;
  return match[1].matched ? match[1].str() : match[2].str();
}

std::optional<std::string> elf_build_id(const std::vector<std::byte>& data) {
  if (data.size() < sizeof(Elf64_Ehdr)) return std::nullopt;
  Elf64_Ehdr header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB ||
      header.e_phentsize != sizeof(Elf64_Phdr))
    return std::nullopt;
  const auto in_bounds = [&](std::uint64_t offset, std::uint64_t size) {
    return offset <= data.size() && size <= data.size() - offset;
  };
  if (!in_bounds(header.e_phoff,
                 static_cast<std::uint64_t>(header.e_phnum) *
                     sizeof(Elf64_Phdr)))
    return std::nullopt;
  for (Elf64_Half index = 0; index < header.e_phnum; ++index) {
    Elf64_Phdr program{};
    std::memcpy(&program, data.data() + header.e_phoff +
                                   static_cast<std::size_t>(index) *
                                       sizeof(Elf64_Phdr),
                sizeof(program));
    if (program.p_type != PT_NOTE ||
        !in_bounds(program.p_offset, program.p_filesz))
      continue;
    std::size_t cursor = static_cast<std::size_t>(program.p_offset);
    const std::size_t end = cursor + static_cast<std::size_t>(program.p_filesz);
    while (cursor + 12 <= end) {
      std::uint32_t namesz = 0;
      std::uint32_t descsz = 0;
      std::uint32_t type = 0;
      std::memcpy(&namesz, data.data() + cursor, sizeof(namesz));
      std::memcpy(&descsz, data.data() + cursor + 4, sizeof(descsz));
      std::memcpy(&type, data.data() + cursor + 8, sizeof(type));
      cursor += 12;
      const std::size_t name_bytes = (static_cast<std::size_t>(namesz) + 3) & ~3U;
      const std::size_t desc_bytes = (static_cast<std::size_t>(descsz) + 3) & ~3U;
      if (name_bytes > end - cursor || desc_bytes > end - cursor - name_bytes)
        break;
      const auto* name = reinterpret_cast<const char*>(data.data() + cursor);
      const std::size_t desc_offset = cursor + name_bytes;
      if (type == NT_GNU_BUILD_ID && namesz >= 3 &&
          std::memcmp(name, "GNU", 3) == 0) {
        std::string result;
        result.reserve(descsz * 2);
        static constexpr char digits[] = "0123456789abcdef";
        for (std::size_t byte = 0; byte < descsz; ++byte) {
          const auto value = static_cast<unsigned char>(
              data[desc_offset + byte]);
          result.push_back(digits[value >> 4]);
          result.push_back(digits[value & 0xf]);
        }
        return result;
      }
      cursor = desc_offset + desc_bytes;
    }
  }
  return std::nullopt;
}

void validate_libroblox_patch_overlay(const std::filesystem::path& path,
                                      const std::vector<std::byte>& original,
                                      const std::vector<std::byte>& patched) {
  const auto manifest_path = path.string() + ".json";
  const auto manifest_bytes = read_file(manifest_path);
  const std::string manifest(reinterpret_cast<const char*>(manifest_bytes.data()),
                             manifest_bytes.size());
  const auto field = [&](const char* name) {
    const auto value = patch_manifest_field(manifest, name);
    if (!value) {
      throw std::runtime_error("libroblox patch manifest lacks " +
                               std::string(name));
    }
    return *value;
  };
  if (field("format") != "1" ||
      field("patch") != "TexturePackGeneratorUseOriginalDefault" ||
      field("target") != "libroblox.so") {
    throw std::runtime_error("unsupported libroblox patch manifest");
  }
  if (field("original_sha256") != sha256_hex(original) ||
      field("patched_sha256") != sha256_hex(patched) ||
      field("original_size") != std::to_string(original.size()) ||
      field("patched_size") != std::to_string(patched.size())) {
    throw std::runtime_error("libroblox patch manifest does not match its images");
  }
  if (field("original_bytes") != "31c0" ||
      field("replacement_bytes") != "b001") {
    throw std::runtime_error("unexpected libroblox patch bytes");
  }
  const auto original_id = elf_build_id(original);
  const auto patched_id = elf_build_id(patched);
  if (!original_id || !patched_id || *original_id != *patched_id ||
      field("build_id") != *original_id) {
    throw std::runtime_error("libroblox patch build ID mismatch");
  }
  if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
    std::fprintf(stderr, "nuah loader: validated libroblox sidecar %s build=%s\n",
                 path.c_str(), patched_id->c_str());
  }
}

void write_all(int fd, const std::vector<std::byte>& data) {
  std::size_t done = 0;
  while (done < data.size()) {
    const auto n = ::write(fd, data.data() + done, data.size() - done);
    if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("temporary ELF write failed"); }
    done += static_cast<std::size_t>(n);
  }
}

std::vector<std::byte> inflate_raw(std::span<const std::byte> compressed, std::size_t expected_size) {
  std::vector<std::byte> result(expected_size);
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef*>(result.data());
  stream.avail_out = static_cast<uInt>(result.size());
  if (::inflateInit2(&stream, -MAX_WBITS) != Z_OK) throw std::runtime_error("cannot initialize APK deflate stream");
  const auto status = ::inflate(&stream, Z_FINISH);
  ::inflateEnd(&stream);
  if (status != Z_STREAM_END || stream.total_out != expected_size || stream.total_in != compressed.size()) {
    throw std::runtime_error("invalid or truncated APK deflate member");
  }
  return result;
}

template <typename Visitor>
void visit_apk_members(const std::filesystem::path& apk, Visitor&& visitor) {
  const MappedFile mapped(apk);
  const auto data = mapped.span();
  const std::size_t first =
      data.size() > 0xffff + 22 ? data.size() - (0xffff + 22) : 0;
  std::size_t eocd = data.size();
  for (std::size_t i = data.size() >= 22 ? data.size() - 22 : 0; i >= first;
       --i) {
    if (u32(data, i) == kEndOfCentralDirectory) {
      eocd = i;
      break;
    }
    if (i == 0) break;
  }
  if (eocd == data.size())
    throw std::runtime_error("APK has no ZIP central directory");

  const auto entries = u16(data, eocd + 10);
  std::size_t pos = u32(data, eocd + 16);
  for (std::uint16_t i = 0; i < entries; ++i) {
    if (u32(data, pos) != kCentralDirectory)
      throw std::runtime_error("invalid ZIP central directory entry");
    const auto method = u16(data, pos + 10);
    const auto compressed_size = u32(data, pos + 20);
    const auto uncompressed_size = u32(data, pos + 24);
    const auto name_len = u16(data, pos + 28);
    const auto extra_len = u16(data, pos + 30);
    const auto comment_len = u16(data, pos + 32);
    const auto local = u32(data, pos + 42);
    const std::size_t next = pos + 46 + name_len + extra_len + comment_len;
    if (next > data.size())
      throw std::runtime_error("truncated ZIP central directory name");
    const std::string name(
        reinterpret_cast<const char*>(data.data() + pos + 46), name_len);

    if (visitor(name, method, compressed_size, uncompressed_size, local,
                data)) {
      return;
    }
    pos = next;
  }
}

std::vector<std::byte> decode_member(std::span<const std::byte> data,
                                     std::uint16_t method,
                                     std::uint32_t compressed_size,
                                     std::uint32_t uncompressed_size,
                                     std::uint32_t local) {
  if (u32(data, local) != kLocalFile)
    throw std::runtime_error("invalid ZIP local header");
  const auto local_name = u16(data, local + 26);
  const auto local_extra = u16(data, local + 28);
  const auto start =
      static_cast<std::size_t>(local) + 30 + local_name + local_extra;
  if (start + compressed_size > data.size())
    throw std::runtime_error("truncated APK member data");
  std::span<const std::byte> encoded(data.data() + start, compressed_size);
  if (method == 0) {
    if (compressed_size != uncompressed_size)
      throw std::runtime_error("stored APK member has inconsistent size");
    return std::vector<std::byte>(encoded.begin(), encoded.end());
  }
  if (method == 8) return inflate_raw(encoded, uncompressed_size);
  throw std::runtime_error("unsupported APK ZIP compression method");
}

std::vector<std::string> needed_libraries(const std::vector<std::byte>& data) {
  validate_elf(data);
  Elf64_Ehdr header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (header.e_phoff > data.size() || header.e_phnum >
      (data.size() - header.e_phoff) / sizeof(Elf64_Phdr)) {
    throw std::runtime_error("ELF program headers are outside the image");
  }
  const Elf64_Phdr* dynamic = nullptr;
  for (std::uint16_t i = 0; i < header.e_phnum; ++i) {
    Elf64_Phdr ph{};
    std::memcpy(&ph, data.data() + header.e_phoff + i * sizeof(ph), sizeof(ph));
    if (ph.p_type == PT_DYNAMIC) {
      dynamic = reinterpret_cast<const Elf64_Phdr*>(data.data() +
                                                     header.e_phoff + i * sizeof(ph));
      break;
    }
  }
  if (!dynamic) return {};
  if (dynamic->p_offset > data.size() || dynamic->p_filesz >
      data.size() - dynamic->p_offset || dynamic->p_filesz % sizeof(Elf64_Dyn)) {
    throw std::runtime_error("ELF dynamic section is outside the image");
  }
  const auto* entries = reinterpret_cast<const Elf64_Dyn*>(
      data.data() + dynamic->p_offset);
  const std::size_t count = dynamic->p_filesz / sizeof(Elf64_Dyn);
  Elf64_Xword strtab_address = 0;
  std::vector<Elf64_Xword> needed_offsets;
  for (std::size_t i = 0; i < count && entries[i].d_tag != DT_NULL; ++i) {
    if (entries[i].d_tag == DT_STRTAB) strtab_address = entries[i].d_un.d_ptr;
    if (entries[i].d_tag == DT_NEEDED) needed_offsets.push_back(entries[i].d_un.d_val);
  }
  if (!strtab_address) return {};
  std::size_t strtab_offset = 0;
  bool found = false;
  for (std::uint16_t i = 0; i < header.e_phnum; ++i) {
    Elf64_Phdr ph{};
    std::memcpy(&ph, data.data() + header.e_phoff + i * sizeof(ph), sizeof(ph));
    if (ph.p_type != PT_LOAD || strtab_address < ph.p_vaddr ||
        strtab_address - ph.p_vaddr >= ph.p_filesz) continue;
    strtab_offset = static_cast<std::size_t>(ph.p_offset +
                                              (strtab_address - ph.p_vaddr));
    found = strtab_offset < data.size();
    break;
  }
  if (!found) throw std::runtime_error("ELF string table is outside the image");
  std::vector<std::string> result;
  for (const auto offset : needed_offsets) {
    const auto at = strtab_offset + static_cast<std::size_t>(offset);
    if (at >= data.size()) throw std::runtime_error("ELF dependency name is outside the image");
    const auto* begin = reinterpret_cast<const char*>(data.data() + at);
    const auto* end = reinterpret_cast<const char*>(data.data() + data.size());
    const auto* nul = static_cast<const char*>(std::memchr(begin, '\0', end - begin));
    if (!nul) throw std::runtime_error("unterminated ELF dependency name");
    result.emplace_back(begin, nul);
  }
  return result;
}

std::vector<std::string> undefined_dynamic_symbols(
    const std::vector<std::byte>& data) {
  validate_elf(data);
  Elf64_Ehdr header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (!header.e_shoff || !header.e_shnum ||
      header.e_shoff > data.size() ||
      header.e_shnum >
          (data.size() - header.e_shoff) / sizeof(Elf64_Shdr)) {
    throw std::runtime_error("ELF section headers are outside the image");
  }

  std::vector<std::string> result;
  for (std::uint16_t index = 0; index < header.e_shnum; ++index) {
    Elf64_Shdr symbols{};
    std::memcpy(&symbols,
                data.data() + header.e_shoff +
                    index * sizeof(Elf64_Shdr),
                sizeof(symbols));
    if (symbols.sh_type != SHT_DYNSYM ||
        symbols.sh_entsize != sizeof(Elf64_Sym) ||
        symbols.sh_link >= header.e_shnum ||
        symbols.sh_offset > data.size() ||
        symbols.sh_size > data.size() - symbols.sh_offset) {
      continue;
    }
    Elf64_Shdr strings{};
    std::memcpy(&strings,
                data.data() + header.e_shoff +
                    symbols.sh_link * sizeof(Elf64_Shdr),
                sizeof(strings));
    if (strings.sh_type != SHT_STRTAB ||
        strings.sh_offset > data.size() ||
        strings.sh_size > data.size() - strings.sh_offset) {
      throw std::runtime_error("ELF dynamic string table is outside the image");
    }
    const auto count = symbols.sh_size / sizeof(Elf64_Sym);
    for (std::size_t symbol_index = 0; symbol_index < count;
         ++symbol_index) {
      Elf64_Sym symbol{};
      std::memcpy(&symbol,
                  data.data() + symbols.sh_offset +
                      symbol_index * sizeof(Elf64_Sym),
                  sizeof(symbol));
      if (symbol.st_shndx != SHN_UNDEF ||
          ELF64_ST_BIND(symbol.st_info) != STB_GLOBAL ||
          !symbol.st_name || symbol.st_name >= strings.sh_size) {
        continue;
      }
      const auto* begin = reinterpret_cast<const char*>(
          data.data() + strings.sh_offset + symbol.st_name);
      const auto* end = reinterpret_cast<const char*>(
          data.data() + strings.sh_offset + strings.sh_size);
      const auto* nul =
          static_cast<const char*>(std::memchr(begin, '\0', end - begin));
      if (!nul) {
        throw std::runtime_error(
            "unterminated ELF dynamic symbol name");
      }
      result.emplace_back(begin, nul);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

void preflight_host_hooks(const std::vector<std::byte>& image,
                          const char* requester) {
  std::vector<std::string> missing;
  for (const auto& symbol : undefined_dynamic_symbols(image)) {
    if (!resolve_host_provider_symbol(symbol.c_str(), requester)) {
      missing.push_back(symbol);
    }
  }
  if (missing.empty()) return;
  std::string message =
      "libhybris preflight found " + std::to_string(missing.size()) +
      " unresolved strong symbol";
  if (missing.size() != 1) message += "s";
  message += ":";
  for (const auto& symbol : missing) message += " " + symbol;
  throw std::runtime_error(message);
}

}  // namespace

ApkMember read_stored_apk_member(const std::filesystem::path& apk, const std::string& member) {
  std::optional<ApkMember> result;
  visit_apk_members(
      apk, [&](const std::string& name, std::uint16_t method,
               std::uint32_t compressed, std::uint32_t uncompressed,
               std::uint32_t local, std::span<const std::byte> data) {
        if (name != member) return false;
        result = ApkMember{
            name, decode_member(data, method, compressed, uncompressed, local)};
        return true;
      });
  if (!result) throw std::runtime_error("APK member not found: " + member);
  return std::move(*result);
}

std::vector<ApkMember> read_apk_members_with_prefix(
    const std::filesystem::path& apk, const std::string& prefix) {
  std::vector<ApkMember> result;
  visit_apk_members(
      apk, [&](const std::string& name, std::uint16_t method,
               std::uint32_t compressed, std::uint32_t uncompressed,
               std::uint32_t local, std::span<const std::byte> data) {
        if (name.starts_with(prefix) && !name.ends_with("/")) {
          result.push_back(ApkMember{
              name,
              decode_member(data, method, compressed, uncompressed, local)});
        }
        return false;
      });
  return result;
}

std::vector<std::string> elf_needed_libraries(
    const std::vector<std::byte>& elf_bytes) {
  return needed_libraries(elf_bytes);
}

void prepend_search_path(const char* name, const std::string& piece) {
  if (piece.empty()) return;
  const char* existing = ::getenv(name);
  if (existing && *existing && std::string(existing).find(piece) != std::string::npos)
    return;
  std::string value = piece;
  if (existing && *existing) {
    value.push_back(':');
    value += existing;
  }
  if (::setenv(name, value.c_str(), 1) != 0) {
    throw std::runtime_error(std::string("cannot configure ") + name);
  }
}

void configure_android_library_path(
    const std::filesystem::path& app_directory) {
  const std::string app_library = (app_directory / "lib").string();
  const std::string app_tree = app_directory.string() + "**";
  prepend_search_path("HYBRIS_LD_LIBRARY_PATH", app_library);
  prepend_search_path("HYBRIS_LD_LIBRARY_PATH", app_tree);
  prepend_search_path("BIONIC_LD_LIBRARY_PATH", app_library);
  prepend_search_path("BIONIC_LD_LIBRARY_PATH", app_tree);

  std::string path = app_library;
  if (const char* configured = ::getenv("BIONIC_LD_LIBRARY_PATH");
      configured && *configured) {
    path += ":";
    path += configured;
  }
  path += ":" + app_directory.string() + "**";
  using ParseLibraryPath = void (*)(const char*, char*);
  ParseLibraryPath parse = nullptr;
  void* linker_handle = nullptr;
  if (bionic_provider_handle) {
    linker_handle = bionic_provider_handle;
    parse = reinterpret_cast<ParseLibraryPath>(
        ::dlsym(bionic_provider_handle, "dl_parse_library_path"));
  }
  if (!parse) {
    parse = reinterpret_cast<ParseLibraryPath>(
        ::dlsym(RTLD_DEFAULT, "dl_parse_library_path"));
  }
  if (!parse) {
    static const char* candidates[] = {
        "/lib64/libdl_bio.so.0", "/usr/lib64/libdl_bio.so.0",
        "/lib/libdl_bio.so.0", "/usr/lib/libdl_bio.so.0"};
    for (const char* candidate : candidates) {
      void* linker = ::dlopen(candidate, RTLD_NOW | RTLD_GLOBAL);
      if (!linker) continue;
      linker_handle = linker;
      parse = reinterpret_cast<ParseLibraryPath>(
          ::dlsym(linker, "dl_parse_library_path"));
      if (parse) break;
      ::dlclose(linker);
    }
  }
  if (parse) {
    char delimiter[] = ":";
    parse(path.data(), delimiter);
  }

  struct r_debug** linker_debug = nullptr;
  if (linker_handle) {
    linker_debug = reinterpret_cast<struct r_debug**>(
        ::dlsym(linker_handle, "_r_debug_ptr"));
  }
  if (!linker_debug) {
    linker_debug = reinterpret_cast<struct r_debug**>(
        ::dlsym(RTLD_DEFAULT, "_r_debug_ptr"));
  }
  if (linker_debug && !*linker_debug) {
    auto* host_debug = reinterpret_cast<struct r_debug*>(
        ::dlsym(RTLD_DEFAULT, "_r_debug"));
    if (host_debug) *linker_debug = host_debug;
  }
}

LoadedModule::~LoadedModule() {
  /* Android keeps application native libraries resident for the lifetime of
   * the process.  Roblox registers a large destructor graph (including its
   * stdio/error-reporting state); unloading it through hybris during the
   * isolated bootstrap teardown runs those destructors after the host FILE
   * domain has changed and can crash in fflush().  The native child exits
   * immediately after reporting its status, so leaking this one app handle
   * is both safer and faithful to Android's process model. */
  const auto filename = path_.filename().string();
  const bool app_library_lifetime = filename == "libroblox.so" ||
                                    filename.starts_with("libroblox.");
  if (!app_library_lifetime && handle_ && close_) close_(handle_);
  if (!app_library_lifetime && loader_library_) ::dlclose(loader_library_);
  if (remove_path_ && !path_.empty()) {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
}
LoadedModule::LoadedModule(LoadedModule&& other) noexcept : path_(std::move(other.path_)), handle_(other.handle_), loader_library_(other.loader_library_), close_(other.close_), symbol_(other.symbol_), size_(other.size_) {
  versioned_symbol_ = other.versioned_symbol_;
  remove_path_ = other.remove_path_;
  other.path_.clear(); other.handle_ = nullptr; other.loader_library_ = nullptr; other.close_ = nullptr; other.symbol_ = nullptr; other.versioned_symbol_ = nullptr; other.size_ = 0; other.remove_path_ = true;
}
LoadedModule& LoadedModule::operator=(LoadedModule&& other) noexcept {
  if (this != &other) { this->~LoadedModule(); path_ = std::move(other.path_); handle_ = other.handle_; loader_library_ = other.loader_library_; close_ = other.close_; symbol_ = other.symbol_; versioned_symbol_ = other.versioned_symbol_; size_ = other.size_; remove_path_ = other.remove_path_; other.path_.clear(); other.handle_ = nullptr; other.loader_library_ = nullptr; other.close_ = nullptr; other.symbol_ = nullptr; other.versioned_symbol_ = nullptr; other.size_ = 0; other.remove_path_ = true; }
  return *this;
}

/* The native bootstrap runs in an isolated child and deliberately uses
 * _exit() after handing its status back to the parent.  That means a
 * LoadedModule destructor cannot reap the staging ELF.  Keep the directory
 * bounded on the next launch, but only touch files created by this loader;
 * callers may still place unrelated files beside them.  RuntimeDataLock
 * serializes launches for the profile, so an old module here is not live. */
void reap_stale_native_modules(const std::filesystem::path& directory) {
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error) return;
  std::size_t removed = 0;
  std::uintmax_t removed_bytes = 0;
  for (std::filesystem::directory_iterator entries(
           directory, std::filesystem::directory_options::skip_permission_denied,
           error);
       entries != std::filesystem::directory_iterator(); entries.increment(error)) {
    if (error) {
      error.clear();
      continue;
    }
    const auto name = entries->path().filename().string();
    if (!name.starts_with("nuah-module-")) continue;
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(entries->path(), file_error) ||
        file_error)
      continue;
    const auto bytes = std::filesystem::file_size(entries->path(), file_error);
    if (file_error) continue;
    if (std::filesystem::remove(entries->path(), file_error) && !file_error) {
      ++removed;
      removed_bytes += bytes;
    }
  }
  if (removed && ::getenv("NUAH_BOOTSTRAP_TRACE")) {
    std::fprintf(stderr, "nuah loader: reaped %zu stale native modules (%llu MiB)\n",
                 removed,
                 static_cast<unsigned long long>(removed_bytes / (1024 * 1024)));
  }
}

void* LoadedModule::symbol(const char* name) const {
  if (!handle_ || !name) return nullptr;
  if (symbol_) {
    if (void* result = symbol_(handle_, name)) return result;
  }
  // Android app images produced by the current Roblox toolchain export their
  // JNI entry points in the LIBROBLOX symbol version.  libhybris exposes the
  // correct lookup as android_dlvsym, while its plain dlsym path misses those
  // entries.  Keep this fallback in the loader so callers do not grow a
  // method-by-method facade.
  return versioned_symbol_ ? versioned_symbol_(handle_, name, "LIBROBLOX")
                          : nullptr;
}

LoadedModule load_apk_library(const std::filesystem::path& apk, const std::string& member) {
  const auto apk_member = read_stored_apk_member(apk, member);
  validate_elf(apk_member.bytes);
  const char* patch_setting = ::getenv("NUAH_LIBROBLOX_PATCH");
  const char* memory_setting = ::getenv("NUAH_LIBROBLOX_MEMORY_PATCH");
  const auto is_enabled = [](const char* value) {
    return value && *value && std::strcmp(value, "0") != 0;
  };
  const bool memory_patch_requested =
      (patch_setting &&
       (std::strcmp(patch_setting, "memory") == 0 ||
        std::strcmp(patch_setting, "in-memory") == 0)) ||
      is_enabled(memory_setting);
  std::vector<std::byte> overlay_bytes;
  const std::filesystem::path overlay_path = [&] {
    const char* configured = patch_setting;
    if (!configured || !*configured) return std::filesystem::path{};
    if (std::strcmp(configured, "memory") == 0 ||
        std::strcmp(configured, "in-memory") == 0)
      return std::filesystem::path{};
    const std::filesystem::path candidate(configured);
    overlay_bytes = read_file(candidate);
    validate_elf(overlay_bytes);
    validate_libroblox_patch_overlay(candidate, apk_member.bytes, overlay_bytes);
    return candidate;
  }();
  const auto& image_bytes = overlay_path.empty() ? apk_member.bytes : overlay_bytes;
  /* The native image is about 116 MiB.  Reuse the app-private extraction
   * prepared by native_runtime when it is present; the old path copied the
   * same bytes into a fresh staging file on every launch, then isolated
   * _exit() left that file behind.  A temporary ELF remains the fallback for
   * callers that do not provide an app data directory. */
  std::filesystem::path staging_root = "/tmp";
  if (const char* app_data = ::getenv("ANDROID_APP_DATA_DIR");
      app_data && *app_data) {
    staging_root = std::filesystem::path(app_data) / ".native-tmp";
  }
  std::error_code staging_error;
  std::filesystem::create_directories(staging_root, staging_error);
  if (staging_error)
    throw std::runtime_error("cannot create native staging directory: " +
                             staging_error.message());
  reap_stale_native_modules(staging_root);
  std::filesystem::path path;
  std::filesystem::path temporary_path;
  bool remove_path = true;
  if (!overlay_path.empty()) {
    path = overlay_path;
    remove_path = false;
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
        trace && *trace) {
      std::fprintf(stderr, "nuah loader: using libroblox sidecar %s\n",
                   path.c_str());
    }
  } else if (const char* app_data = ::getenv("ANDROID_APP_DATA_DIR");
      app_data && *app_data) {
    const std::size_t separator = member.find_last_of('/');
    const std::string basename = separator == std::string::npos
                                     ? member
                                     : member.substr(separator + 1);
    const auto app_path = std::filesystem::path(app_data) / "lib" / basename;
    std::error_code app_stat;
    if (std::filesystem::is_regular_file(app_path, app_stat) &&
        std::filesystem::file_size(app_path, app_stat) == image_bytes.size()) {
      path = app_path;
      remove_path = false;
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
          trace && *trace) {
        std::fprintf(stderr, "nuah loader: reusing extracted native image %s\n",
                     path.c_str());
      }
    }
  }
  int fd = -1;
  try {
    if (path.empty()) {
      const std::string pattern =
          (staging_root / "nuah-module-XXXXXX").string();
      std::vector<char> path_template(pattern.begin(), pattern.end());
      path_template.push_back('\0');
      fd = ::mkstemp(path_template.data());
      if (fd < 0) throw std::runtime_error("temporary ELF file creation failed");
      temporary_path = std::filesystem::path(path_template.data());
      path = temporary_path;
      write_all(fd, image_bytes);
      if (::fchmod(fd, 0500) != 0)
        throw std::runtime_error("temporary ELF permission setup failed");
      if (::close(fd) != 0)
        throw std::runtime_error("temporary ELF close failed");
      fd = -1;
    }
    void* loader_library = nullptr;
    const std::string library = hybris_common_library().string();
    configure_hybris_environment(library.c_str());
    if (const char* app_directory = ::getenv("ANDROID_APP_DATA_DIR");
        app_directory && *app_directory) {
      configure_android_library_path(app_directory);
    }
    loader_library = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!loader_library) {
      const char* error = ::dlerror();
      throw std::runtime_error("cannot load libhybris common library: " +
                               std::string(error ? error : "unknown error"));
    }
    const auto android_dlopen = reinterpret_cast<void* (*)(const char*, int)>(
        ::dlsym(loader_library, "android_dlopen"));
    const auto android_dlerror = reinterpret_cast<char* (*)()>(
        ::dlsym(loader_library, "android_dlerror"));
    const auto android_dlclose = reinterpret_cast<int (*)(void*)>(
        ::dlsym(loader_library, "android_dlclose"));
    const auto android_dlsym = reinterpret_cast<void* (*)(void*, const char*)>(
        ::dlsym(loader_library, "android_dlsym"));
    const auto android_dlvsym = reinterpret_cast<void* (*)(
        void*, const char*, const char*)>(
        ::dlsym(loader_library, "android_dlvsym"));
    if (!android_dlopen || !android_dlerror || !android_dlclose || !android_dlsym) {
      ::dlclose(loader_library);
      throw std::runtime_error("libhybris common library lacks Android loader entrypoints");
    }
    configure_host_provider_hooks(loader_library);
    preflight_host_hooks(image_bytes, path.c_str());
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
      void* libc_handle = android_dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
      if (libc_handle) {
        using PropertyGet = int (*)(const char*, char*);
        const auto property = reinterpret_cast<PropertyGet>(
            android_dlsym(libc_handle, "__system_property_get"));
        char value[93]{};
        const int length = property
            ? property("ro.build.version.sdk", value)
            : -1;
        std::fprintf(stderr,
                     "nuah loader: Android libc property=%p sdk=%d value=%s\n",
                     reinterpret_cast<void*>(property), length, value);
        (void)android_dlclose(libc_handle);
      } else {
        std::fprintf(stderr, "nuah loader: Android libc lookup failed: %s\n",
                     android_dlerror ? android_dlerror() : "unknown");
      }
    }
    // ATL's Java System.loadLibrary uses the process-wide bionic linker. If
    // Nuah first opens the image through libhybris, ATL sees a different
    // linker namespace and loads a second copy (JNI_OnLoad then recurses).
    // Prefer the already-installed bionic loader for app DSOs so both calls
    // share one handle; retain libhybris as a fallback for stripped-down
    // installations that do not expose bionic_dlopen.
    using BionicDlopen = void* (*)(const char*, int);
    using BionicDlerror = const char* (*)();
    using BionicDlclose = int (*)(void*);
    using BionicDlsym = void* (*)(void*, const char*);
    const auto bionic_dlopen = reinterpret_cast<BionicDlopen>(
        ::dlsym(RTLD_DEFAULT, "bionic_dlopen"));
    const auto bionic_dlerror = reinterpret_cast<BionicDlerror>(
        ::dlsym(RTLD_DEFAULT, "bionic_dlerror"));
    const auto bionic_dlclose = reinterpret_cast<BionicDlclose>(
        ::dlsym(RTLD_DEFAULT, "bionic_dlclose"));
    const auto bionic_dlsym = reinterpret_cast<BionicDlsym>(
        ::dlsym(RTLD_DEFAULT, "bionic_dlsym"));
    bool bionic_handle = false;
    void* handle = nullptr;
    if (bionic_dlopen && bionic_dlclose && bionic_dlsym) {
      // The Nuah libandroid provider is intentionally local while libhybris
      // wires the app image.  Promote that already-loaded glibc object only
      // at this boundary so bionic_translation's try_glibc fallback can
      // satisfy DT_NEEDED("libandroid.so") by soname.  Do not pass the host
      // ELF to bionic_dlopen itself; its Android relocation parser rejects
      // glibc IFUNC/TLS relocations.
      const auto host_android = runtime_directory() / "android" / "libandroid.so";
      if (std::filesystem::is_regular_file(host_android)) {
        void* promoted = ::dlopen(host_android.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE");
            trace && *trace) {
          std::fprintf(stderr,
                       "nuah loader: promoted host libandroid handle=%p path=%s\n",
                       promoted, host_android.c_str());
        }
      }
      handle = bionic_dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      bionic_handle = handle != nullptr;
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
        const char* error = bionic_dlerror ? bionic_dlerror() : nullptr;
        std::fprintf(stderr,
                     "nuah loader: bionic_dlopen(%s) handle=%p error=%s\n",
                     path.c_str(), handle, error ? error : "none");
      }
    }
    if (!handle) {
      handle = android_dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
        std::fprintf(stderr,
                     "nuah loader: android_dlopen(%s) handle=%p error=%s\n",
                     path.c_str(), handle,
                     android_dlerror ? android_dlerror() : "unknown");
      }
    }
    if (!handle) {
      std::string message = "android_dlopen failed: ";
      const char* error = bionic_handle
                              ? (bionic_dlerror ? bionic_dlerror() : nullptr)
                              : android_dlerror();
      message += error ? error : "unknown loader error";
      const auto needed = needed_libraries(apk_member.bytes);
      if (!needed.empty()) {
        message += " (Android image needs";
        for (const auto& library : needed) message += " " + library;
        message += "; libhybris host hooks could not resolve the image)";
      }
      if (loader_library) ::dlclose(loader_library);
      throw std::runtime_error(message);
    }
    LoadedModule result; result.path_ = path; result.handle_ = handle; result.loader_library_ = loader_library; result.close_ = bionic_handle ? bionic_dlclose : android_dlclose; result.symbol_ = bionic_handle ? bionic_dlsym : android_dlsym; result.versioned_symbol_ = bionic_handle ? nullptr : android_dlvsym; result.size_ = image_bytes.size(); result.remove_path_ = remove_path;
    // This is intentionally after dlopen has relocated the image and before
    // control returns to native_runtime/ART.  No APK bytes or sidecar are
    // changed: only the mapped instruction page is made writable for the
    // two-byte A/B, then returned to its original RX permissions.
    if (memory_patch_requested && !patch_loaded_module_texture_flag_impl(result)) {
      throw std::runtime_error(
          "requested in-memory libroblox texture patch could not be applied");
    }
    if (const char* trace = ::getenv("NUAH_BOOTSTRAP_TRACE"); trace && *trace) {
      const auto lookup = bionic_handle ? bionic_dlsym : android_dlsym;
      if (lookup) {
        using PropertyGet = int (*)(const char*, char*);
        const auto property = reinterpret_cast<PropertyGet>(
            lookup(handle, "__system_property_get"));
        if (property) {
          char value[93]{};
          const int length = property("ro.build.version.sdk", value);
          std::fprintf(stderr,
                       "nuah loader: libroblox __system_property_get=%p sdk=%d value=%s\n",
                       reinterpret_cast<void*>(property), length, value);
        } else {
          std::fprintf(stderr,
                       "nuah loader: libroblox __system_property_get unresolved\n");
        }
      }
    }
    return result;
  } catch (...) {
    if (fd >= 0) ::close(fd);
    std::error_code error;
    std::filesystem::remove(temporary_path, error);
    throw;
  }
}
}  // namespace nuah

namespace nuah {
bool patch_loaded_module_property_import(const LoadedModule& module) {
  return patch_loaded_module_property_import_impl(module);
}

bool patch_loaded_module_texture_flag(const LoadedModule& module) {
  return patch_loaded_module_texture_flag_impl(module);
}

namespace {

void set_if_unset(const char* name, const std::string& value) {
  if (value.empty()) return;
  const char* existing = std::getenv(name);
  if (existing && *existing) return;
  if (::setenv(name, value.c_str(), 1) != 0) {
    throw std::runtime_error(std::string("cannot configure ") + name);
  }
}

void prepend_path_env(const char* name, const std::filesystem::path& item) {
  if (item.empty()) return;
  std::error_code error;
  if (!std::filesystem::exists(item, error) || error) return;
  const std::string piece = item.string();
  const char* existing = std::getenv(name);
  if (existing && *existing &&
      std::string(existing).find(piece) != std::string::npos)
    return;
  std::string value = piece;
  if (existing && *existing) {
    value.push_back(':');
    value += existing;
  }
  if (::setenv(name, value.c_str(), 1) != 0) {
    throw std::runtime_error(std::string("cannot configure ") + name);
  }
}

}  // namespace

void apply_native_host_environment() {
  const auto hybris = hybris_common_library();
  std::error_code hybris_error;
  if (!std::filesystem::exists(hybris, hybris_error) || hybris_error) {
    throw std::runtime_error(
        "libhybris is missing at " + hybris.string() +
        " (expected beside the binary as hybris/lib/libhybris-common.so; "
        "set NUAH_HYBRIS_LIBRARY to override)");
  }
  set_if_unset("NUAH_HYBRIS_LIBRARY", hybris.string());
  configure_hybris_environment(hybris.c_str());
  prepend_path_env("LD_LIBRARY_PATH", hybris.parent_path());
  std::error_code real_error;
  const auto real = std::filesystem::canonical(hybris, real_error);
  std::cerr << "nuah runtime: hybris=" << hybris;
  if (!real_error && real != hybris) std::cerr << " -> " << real;
  std::cerr << '\n';

  const auto root = runtime_directory();
  prepend_path_env("LD_LIBRARY_PATH", root);

  std::filesystem::path art_library = "/usr/local/lib64/art";
  if (const char* configured = std::getenv("NUAH_ART_LIBRARY");
      configured && *configured) {
    art_library = std::filesystem::path(configured).parent_path();
  } else if (const char* configured = std::getenv("NUAH_ART_LIBRARY_DIR");
             configured && *configured) {
    art_library = configured;
  }
  prepend_path_env("LD_LIBRARY_PATH", art_library);
  prepend_path_env("LD_LIBRARY_PATH", art_library / "natives");
  prepend_path_env("LD_PRELOAD", art_library / "libandroidfw.so");
  for (const char* icu : {"libicudata.so.77", "libicuuc.so.77",
                          "libicui18n.so.77"}) {
    prepend_path_env("LD_PRELOAD", art_library / icu);
  }

  prepend_path_env("LD_PRELOAD", "/usr/lib64/libpng16.so.16");
  prepend_path_env("LD_PRELOAD", "/usr/lib64/libjpeg.so.62");

  constexpr const char* kArtHome =
      "/usr/local/lib64/java/dex/android_translation_layer";
  set_if_unset("NUAH_ART_HOME", kArtHome);
  set_if_unset("NUAH_ATL_HOME", kArtHome);
  set_if_unset("NUAH_GRAPHICS_BACKEND", "vulkan");
  set_if_unset("NUAH_VULKAN_PRESENT_MODE", "fifo");
  set_if_unset(
      "NUAH_CLIENT_SETTINGS_JSON",
      "{\"applicationSettings\":{\"DFFlagDebugDisableRbxTransportDummyClient\":true}}");
}

}  // namespace nuah
