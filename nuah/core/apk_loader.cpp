#include "nuah/apk_loader.hpp"
#include "nuah/sha256.h"
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "nuah/bootstrap_diagnostics.h"
namespace nuah {
namespace {
constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kCentralDirectory = 0x02014b50;
constexpr std::uint32_t kLocalFile = 0x04034b50;
template <typename... Args>
inline bool streq_any(const char* s, Args... targets) {
  return ((std::strcmp(s, targets) == 0) || ...);
}
// Late-exported toggles are read via getenv on every call; one helper keeps sites aligned.
inline const char* getenv_value(const char* name) {
  const char* v = ::getenv(name);
  return (v && *v) ? v : nullptr;
}
inline bool bootstrap_trace_enabled() { return getenv_value("NUAH_BOOTSTRAP_TRACE") != nullptr; }
template <typename... A>
inline void bootstrap_trace(const char* fmt, A... args) {
  if (bootstrap_trace_enabled()) std::fprintf(stderr, fmt, args...);
}
inline bool env_flag(const char* name, bool default_on = false) {
  const char* v = ::getenv(name);
  if (!v || !*v) return default_on;
  return std::strcmp(v, "0") != 0;
}
[[noreturn]] inline void fail(const std::string& m) { throw std::runtime_error(m); }
[[noreturn]] inline void throw_dlerror(std::string prefix) {
  const char* e = ::dlerror();
  prefix += ": ";
  prefix += e ? e : "unknown error";
  throw std::runtime_error(prefix);
}
// Shared ELF bounds/header check for the DynSym/group scans below.
inline bool elf_in_bounds(const std::vector<std::byte>& b, std::uint64_t o, std::uint64_t s) { return o <= b.size() && s <= b.size() - o; }
inline bool read_elf_header(const std::vector<std::byte>& b, Elf64_Ehdr& h) {
  if (b.size() < sizeof(h)) return false;
  std::memcpy(&h, b.data(), sizeof(h));
  return std::memcmp(h.e_ident, ELFMAG, SELFMAG) == 0 && h.e_ident[EI_CLASS] == ELFCLASS64;
}
using PropertyGetFn = int (*)(const char*, char*);
// Trace-only probe: shows which __system_property_get an image resolved (API-36 façade vs stale SDK).
inline void trace_sdk_property(const char* label, void* property_fn) {
  char value[93]{};
  const auto property = reinterpret_cast<PropertyGetFn>(property_fn);
  const int length = property ? property("ro.build.version.sdk", value) : -1;
  std::fprintf(stderr, "nuah loader: %s=%p sdk=%d value=%s\n", label, property_fn, length, value);
}
std::filesystem::path runtime_directory() {
  std::array<char, 4096> path{};
  const auto size = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size <= 0 || static_cast<std::size_t>(size) >= path.size() - 1) fail("cannot locate Nuah runtime directory");
  return std::filesystem::path(std::string(path.data(), size)).parent_path();
}
void set_hybris_path_if_unset(const char* name, const std::string& value) {
  if (value.empty() || getenv_value(name)) return;
  if (::setenv(name, value.c_str(), 1) != 0) fail(std::string("cannot configure ") + name);
}
// Only the tree beside the binary or an explicit override; never ~/.local or /usr/local.
std::filesystem::path hybris_common_library() {
  if (auto* c = getenv_value("NUAH_HYBRIS_LIBRARY")) return c;
  if (auto* c = getenv_value("NUAH_HYBRIS_LIBRARY_DIR")) return std::filesystem::path(c) / "libhybris-common.so";
  return runtime_directory() / "hybris" / "lib" / "libhybris-common.so";
}
void configure_hybris_environment(const char* library) {
  // Only dependency placeholders go on the Android search path; real providers are host-loaded below.
  const auto root = runtime_directory() / "android";
  const auto linker_dependencies = root / "linker-deps";
  const auto linker_path = std::filesystem::is_directory(linker_dependencies) ? linker_dependencies : root;  // compatibility with old artifacts
  set_hybris_path_if_unset("HYBRIS_LD_LIBRARY_PATH", linker_path.string());
  if (!library || !*library || getenv_value("HYBRIS_LINKER_DIR")) return;
  const std::filesystem::path common(library);
  if (!common.has_parent_path()) return;
  set_hybris_path_if_unset("HYBRIS_LINKER_DIR", (common.parent_path() / "libhybris" / "linker").string());
}
std::vector<void*> host_provider_handles;
void* bionic_provider_handle = nullptr;
void* android_provider_handle = nullptr;
// Optional pinned pthread wrapper: stores the host pointer in the Android object (low-bit tag).
void* pthread_bridge_handle = nullptr;
void* host_libc_handle = nullptr;
// Keep the installed ATL/Bionic DSO and its TLS domain intact; only relocate the app-local property.
extern "C" int nuah_api36_system_property_get(const char* key, char* value) {
  static constexpr std::pair<const char*, const char*> kProperties[] = {
      {"ro.build.version.sdk", "36"}, {"ro.build.version.release", "10"}, {"ro.product.cpu.abi", "x86_64"}};
  const char* result = "";
  if (key)
    for (const auto& [name, text] : kProperties)
      if (std::strcmp(key, name) == 0) { result = text; break; }
  if (value) std::strcpy(value, result);
  if (getenv_value("NUAH_PROPERTY_TRACE")) std::fprintf(stderr, "nuah property: %s -> %s\n", key ? key : "(null)", result);
  return static_cast<int>(std::strlen(result));
}
std::vector<std::byte> read_file(const std::filesystem::path& path);
// Shared DYNSYM view for the two app-local patches; both derive bias from an exported anchor.
struct DynSymView {
  const Elf64_Sym* symbols = nullptr;
  std::size_t count = 0;
  const char* strings = nullptr;
  std::size_t strings_size = 0;
};
inline bool dynsym_view(const std::vector<std::byte>& bytes, const Elf64_Ehdr& header, DynSymView* out) {
  if (header.e_shentsize != sizeof(Elf64_Shdr) || !elf_in_bounds(bytes, header.e_shoff, static_cast<std::uint64_t>(header.e_shnum) * sizeof(Elf64_Shdr))) return false;
  const auto* sections = reinterpret_cast<const Elf64_Shdr*>(bytes.data() + header.e_shoff);
  for (Elf64_Half i = 0; i < header.e_shnum; ++i) {
    const auto& s = sections[i];
    if (s.sh_type != SHT_DYNSYM || s.sh_entsize != sizeof(Elf64_Sym) || !elf_in_bounds(bytes, s.sh_offset, s.sh_size) || s.sh_link >= header.e_shnum) continue;
    const auto& t = sections[s.sh_link];
    if (t.sh_type != SHT_STRTAB || !elf_in_bounds(bytes, t.sh_offset, t.sh_size)) continue;
    out->symbols = reinterpret_cast<const Elf64_Sym*>(bytes.data() + s.sh_offset);
    out->count = s.sh_size / sizeof(Elf64_Sym);
    out->strings = reinterpret_cast<const char*>(bytes.data() + t.sh_offset);
    out->strings_size = t.sh_size;
    return true;
  }
  return false;
}
inline bool dynsym_value(const DynSymView& view, const char* name, std::uint64_t* out) {
  for (std::size_t i = 0; i < view.count; ++i) {
    const auto& s = view.symbols[i];
    if (s.st_name >= view.strings_size || s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
    if (std::strcmp(view.strings + s.st_name, name) == 0) {
      *out = s.st_value;
      return true;
    }
  }
  return false;
}
// Shared PT_LOAD/PT_DYNAMIC scans (property GOT, texture site, preflight).
inline bool copy_program_headers(const std::vector<std::byte>& b, const Elf64_Ehdr& h, std::vector<Elf64_Phdr>& out) {
  if (h.e_phentsize != sizeof(Elf64_Phdr) || !elf_in_bounds(b, h.e_phoff, static_cast<std::uint64_t>(h.e_phnum) * sizeof(Elf64_Phdr))) return false;
  out.resize(h.e_phnum);
  std::memcpy(out.data(), b.data() + h.e_phoff, out.size() * sizeof(Elf64_Phdr));
  return true;
}
inline std::vector<Elf64_Phdr> load_segments(const std::vector<Elf64_Phdr>& programs, const std::vector<std::byte>& b) {
  std::vector<Elf64_Phdr> loads;
  loads.reserve(programs.size());
  for (const auto& p : programs)
    if (p.p_type == PT_LOAD && elf_in_bounds(b, p.p_offset, p.p_filesz)) loads.push_back(p);
  return loads;
}
inline const Elf64_Phdr* find_dynamic_segment(const Elf64_Phdr* phdrs, Elf64_Half n) {
  for (Elf64_Half i = 0; i < n; ++i)
    if (phdrs[i].p_type == PT_DYNAMIC) return &phdrs[i];
  return nullptr;
}
inline bool va_to_file_offset(const Elf64_Phdr* phdrs, Elf64_Half n, const std::vector<std::byte>& b, std::uint64_t addr, std::uint64_t size, std::uint64_t* out) {
  for (Elf64_Half i = 0; i < n; ++i) {
    const auto& p = phdrs[i];
    if (p.p_type != PT_LOAD || addr < p.p_vaddr) continue;
    const std::uint64_t d = addr - p.p_vaddr;
    if (d <= p.p_filesz && size <= p.p_filesz - d) {
      *out = p.p_offset + d;
      return elf_in_bounds(b, *out, size);
    }
  }
  return false;
}
inline std::optional<std::uint64_t> file_to_va_addr(const std::vector<Elf64_Phdr>& loads, std::size_t off) {
  for (const auto& p : loads)
    if (off >= p.p_offset && static_cast<std::uint64_t>(off) - p.p_offset < p.p_filesz) return p.p_vaddr + (static_cast<std::uint64_t>(off) - p.p_offset);
  return std::nullopt;
}
bool patch_loaded_module_property_import_impl(const LoadedModule& module) {
#if !defined(__x86_64__)
  (void)module;
  return false;
#else
  // Derive bias from file tables, not bionic_dladdr (hybris trampoline ABI differs; PRE_JNI_BIND crash).
  static constexpr const char* kAnchors[] = {"Java_com_google_androidgamesdk_GameActivity_initializeNativeCode", "JNI_OnLoad"};
  void* anchor = nullptr;
  const char* anchor_name = kAnchors[0];
  for (const char* name : kAnchors) {
    anchor = module.symbol(name);
    if (anchor) { anchor_name = name; break; }
  }
  if (!anchor || module.path().empty()) return false;
  const auto bytes = read_file(module.path());
  Elf64_Ehdr header{};
  if (!read_elf_header(bytes, header) || header.e_phentsize != sizeof(Elf64_Phdr) || header.e_shentsize != sizeof(Elf64_Shdr) ||
      !elf_in_bounds(bytes, header.e_phoff, static_cast<std::uint64_t>(header.e_phnum) * sizeof(Elf64_Phdr)))
    return false;
  DynSymView view{};
  if (!dynsym_view(bytes, header, &view)) return false;
  std::uint64_t anchor_value = 0;
  if (!dynsym_value(view, anchor_name, &anchor_value)) return false;
  const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(bytes.data() + header.e_phoff);
  const auto base = reinterpret_cast<std::uintptr_t>(anchor) - anchor_value;
  const auto page_size = static_cast<std::uintptr_t>(::getpagesize());
  unsigned char resident = 0;
  if (::mincore(reinterpret_cast<void*>(base & ~(page_size - 1)), page_size, &resident) != 0) return false;
  const Elf64_Phdr* dynamic_segment = find_dynamic_segment(phdrs, header.e_phnum);
  if (!dynamic_segment || !elf_in_bounds(bytes, dynamic_segment->p_offset, dynamic_segment->p_filesz)) return false;
  const Elf64_Dyn* dynamic = reinterpret_cast<const Elf64_Dyn*>(bytes.data() + dynamic_segment->p_offset);
  const std::size_t dynamic_count = dynamic_segment->p_filesz / sizeof(Elf64_Dyn);
  std::uint64_t jmprel_va = 0;
  std::size_t jmprel_size = 0;
  for (std::size_t index = 0; index < dynamic_count; ++index) {
    if (dynamic[index].d_tag == DT_NULL) break;
    if (dynamic[index].d_tag == DT_JMPREL) jmprel_va = dynamic[index].d_un.d_ptr;
    else if (dynamic[index].d_tag == DT_PLTRELSZ) jmprel_size = dynamic[index].d_un.d_val;
  }
  if (!jmprel_va || jmprel_size == 0 || jmprel_size % sizeof(Elf64_Rela) != 0) return false;
  std::uint64_t rel_offset = 0;
  if (!va_to_file_offset(phdrs, header.e_phnum, bytes, jmprel_va, jmprel_size, &rel_offset)) return false;
  const auto* relocations = reinterpret_cast<const Elf64_Rela*>(bytes.data() + rel_offset);
  const std::size_t relocation_count = jmprel_size / sizeof(Elf64_Rela);
  for (std::size_t index = 0; index < relocation_count; ++index) {
    const auto& relocation = relocations[index];
    if (ELF64_R_TYPE(relocation.r_info) != R_X86_64_JUMP_SLOT) continue;
    const auto symbol_index = ELF64_R_SYM(relocation.r_info);
    if (symbol_index >= view.count || view.symbols[symbol_index].st_name >= view.strings_size) continue;
    if (std::strcmp(view.strings + view.symbols[symbol_index].st_name, "__system_property_get") != 0) continue;
    const auto slot_address = base + relocation.r_offset;
    const auto page = slot_address & ~(page_size - 1);
    if (::mincore(reinterpret_cast<void*>(page), page_size, &resident) != 0 || ::mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_WRITE) != 0) return false;
    *reinterpret_cast<void**>(slot_address) = reinterpret_cast<void*>(&nuah_api36_system_property_get);
    if (bootstrap_trace_enabled()) {
      char value[93]{};
      const int length = nuah_api36_system_property_get("ro.build.version.sdk", value);
      std::fprintf(stderr, "nuah loader: patched libroblox property GOT=%p sdk=%d value=%s\n", reinterpret_cast<void*>(slot_address), length, value);
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
std::optional<TexturePatchSite> find_texture_patch_site(const std::vector<std::byte>& bytes) {
#if !defined(__x86_64__)
  (void)bytes;
  return std::nullopt;
#else
  Elf64_Ehdr header{};
  if (!read_elf_header(bytes, header) || header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64) return std::nullopt;
  std::vector<Elf64_Phdr> programs;
  if (!copy_program_headers(bytes, header, programs)) return std::nullopt;
  const auto loads = load_segments(programs, bytes);
  if (loads.empty()) return std::nullopt;
  const auto file_to_va = [&](std::size_t off) { return file_to_va_addr(loads, off); };
  static constexpr char target_string[] = "TexturePackGeneratorUseOriginal";
  const std::size_t target_length = sizeof(target_string) - 1;
  std::vector<std::uint64_t> target_addresses;
  for (std::size_t offset = 0; offset + target_length <= bytes.size(); ++offset) {
    if (std::memcmp(bytes.data() + offset, target_string, target_length) != 0) continue;
    if (const auto address = file_to_va(offset)) target_addresses.push_back(*address);
  }
  if (target_addresses.empty()) return std::nullopt;
  const auto has_target = [&](std::uint64_t address) { return std::find(target_addresses.begin(), target_addresses.end(), address) != target_addresses.end(); };
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
      const bool original = std::memcmp(bytes.data() + candidate, prefix, sizeof(prefix)) == 0;
      const bool patched = std::memcmp(bytes.data() + candidate, patched_prefix, sizeof(patched_prefix)) == 0;
      if (!original && !patched) continue;
      const std::size_t lea = candidate + 8;
      if (std::memcmp(bytes.data() + lea, lea_prefix, sizeof(lea_prefix)) != 0) continue;
      std::int32_t displacement = 0;
      std::memcpy(&displacement, bytes.data() + lea + 3, sizeof(displacement));
      const auto lea_address = file_to_va(lea);
      if (!lea_address) continue;
      const auto target_address = static_cast<std::int64_t>(*lea_address) + 7 + static_cast<std::int64_t>(displacement);
      if (target_address < 0 || !has_target(static_cast<std::uint64_t>(target_address))) continue;
      const std::size_t marker = candidate + 22;
      if (std::memcmp(bytes.data() + marker, marker_prefix, sizeof(marker_prefix)) != 0 || std::memcmp(bytes.data() + marker + 7, "\x1f\x00\x00\x00", 4) != 0) continue;
      const auto instruction_address = file_to_va(candidate);
      if (!instruction_address) continue;
      const int prot = ((program.p_flags & PF_R) ? PROT_READ : 0) | ((program.p_flags & PF_W) ? PROT_WRITE : 0) | ((program.p_flags & PF_X) ? PROT_EXEC : 0);
      matches.push_back(TexturePatchSite{candidate, *instruction_address, prot, patched});
    }
  }
  if (matches.size() != 1) return std::nullopt;
  return matches.front();
#endif
}
std::optional<std::uintptr_t> module_load_bias_for_anchor(const LoadedModule& module, const std::vector<std::byte>& bytes, const char* anchor_name) {
#if !defined(__x86_64__)
  (void)module;
  (void)bytes;
  (void)anchor_name;
  return std::nullopt;
#else
  void* anchor = module.symbol(anchor_name);
  Elf64_Ehdr header{};
  if (!anchor || !read_elf_header(bytes, header)) return std::nullopt;
  DynSymView view{};
  if (!dynsym_view(bytes, header, &view)) return std::nullopt;
  std::uint64_t symbol_value = 0;
  if (!dynsym_value(view, anchor_name, &symbol_value)) return std::nullopt;
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
    bootstrap_trace("nuah loader: libroblox texture patch signature not found or ambiguous\n");
    return false;
  }
  std::optional<std::uintptr_t> load_bias;
  static constexpr const char* anchors[] = {"Java_com_google_androidgamesdk_GameActivity_initializeNativeCode", "JNI_OnLoad"};
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
  if (::mincore(reinterpret_cast<void*>(page), page_size, &resident) != 0) return false;
  auto* instruction = reinterpret_cast<unsigned char*>(target);
  if (site->already_patched) {
    bootstrap_trace("nuah loader: libroblox texture default already enabled in memory at %p\n", static_cast<void*>(instruction));
    return true;
  }
  if (instruction[0] != 0x31 || instruction[1] != 0xc0) return false;
  if (::mprotect(reinterpret_cast<void*>(page), page_size, site->original_protection | PROT_WRITE) != 0) {
    bootstrap_trace("nuah loader: texture page mprotect(RWX) failed: %s\n", std::strerror(errno));
    return false;
  }
  const unsigned char replacement[] = {0xb0, 0x01};
  std::memcpy(instruction, replacement, sizeof(replacement));
  __builtin___clear_cache(reinterpret_cast<char*>(instruction), reinterpret_cast<char*>(instruction + sizeof(replacement)));
  if (::mprotect(reinterpret_cast<void*>(page), page_size, site->original_protection) != 0) {
    bootstrap_trace("nuah loader: texture page permission restore failed: %s\n", std::strerror(errno));
    return false;
  }
  bootstrap_trace("nuah loader: patched libroblox texture default in memory at %p (file+0x%zx)\n", static_cast<void*>(instruction), site->file_offset);
  return true;
#endif
}
// Production boundary is libhybris (owns linker ABI + libc/pthread/TLS).
long android_sysconf(int name) {
  // Bionic sysconf names are ABI numbers; forwarding raw ints to glibc misinterprets them (e.g. 0x27).
  static const std::pair<int, int> kSysconfMap[] = {
      {0x0006, _SC_CLK_TCK},        {0x000b, _SC_OPEN_MAX},         {0x0026, _SC_IOV_MAX},        {0x0027, _SC_PAGESIZE},
      {0x0028, _SC_PAGESIZE},       {0x0060, _SC_NPROCESSORS_CONF}, {0x0061, _SC_NPROCESSORS_ONLN}, {0x0062, _SC_PHYS_PAGES},
      {0x0063, _SC_AVPHYS_PAGES},
  };
  for (const auto& [abi, host] : kSysconfMap)
    if (name == abi) return ::sysconf(host);
  errno = EINVAL;
  return -1;
}
void* resolve_host_provider_symbol(const char* symbol, const char* requester) {
  if (!symbol) return nullptr;
  // Narrow override: distro libc reports SDK 16; keep API-36 façade. TLS/libc ownership stays with libhybris/ATL.
  if (std::strcmp(symbol, "__system_property_get") == 0) return reinterpret_cast<void*>(&nuah_api36_system_property_get);
  // Post-InitParams Android pthread layouts use the out-of-line adapter; loader/JNI_OnLoad use native storage.
  const bool android_sync = env_flag("NUAH_ANDROID_SYNC");
  const bool bridge_rwlock = android_sync && env_flag("NUAH_PTHREAD_RWLOCK", true);
  const bool trace_provider = env_flag("NUAH_TRACE_PROVIDER");
  const bool is_pthread_symbol = std::strncmp(symbol, "pthread_", 8) == 0 || std::strncmp(symbol, "__pthread_", 10) == 0;
  const auto traced_provider = [&](const char* owner, void* address) {
    if (trace_provider && is_pthread_symbol) std::fprintf(stderr, "nuah provider: symbol=%s owner=%s address=%p requester=%s\n", symbol, owner, address, requester ? requester : "(unknown)");
    return address;
  };
  // Nuah owns the ANativeWindow family (SDL façade); must precede RTLD_DEFAULT or ATL treats it as GtkWidget.
  const bool native_window_symbol = streq_any(symbol, "ANativeWindow_fromSurface", "ANativeWindow_acquire", "ANativeWindow_release", "ANativeWindow_getWidth", "ANativeWindow_getHeight");
  if (native_window_symbol && android_provider_handle)
    if (void* resolved = ::dlsym(android_provider_handle, symbol)) return traced_provider("nuah-libandroid-window", resolved);
  // Nuah provider is RTLD_LOCAL, invisible via RTLD_DEFAULT; resolve AAsset family via its handle.
  if (android_provider_handle && std::strncmp(symbol, "AAsset", 6) == 0)
    if (void* resolved = ::dlsym(android_provider_handle, symbol)) return traced_provider("nuah-libandroid-assets", resolved);
  // Route bionic_ EGL/GLES entry points via the Nuah provider before host Mesa.
  if (android_provider_handle && (std::strncmp(symbol, "egl", 3) == 0 || std::strncmp(symbol, "gl", 2) == 0)) {
    char bionic_name[128];
    std::snprintf(bionic_name, sizeof(bionic_name), "bionic_%s", symbol);
    if (void* resolved = ::dlsym(android_provider_handle, bionic_name)) { return resolved; }
  }
  // Audited pre-hybris set: API-36 layout differs (pthread attrs, legacy FILE) or carries diagnostics.
  const bool requires_bionic_sync =
      android_sync && (streq_any(symbol, "pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_unlock", "pthread_mutexattr_init",
                                 "pthread_mutexattr_destroy", "pthread_mutexattr_settype", "pthread_cond_init", "pthread_cond_destroy", "pthread_cond_signal", "pthread_cond_broadcast",
                                 "pthread_cond_wait", "pthread_cond_timedwait", "pthread_condattr_init", "pthread_condattr_destroy", "pthread_condattr_setclock") ||
                       (bridge_rwlock && streq_any(symbol, "pthread_rwlock_init", "pthread_rwlock_destroy", "pthread_rwlock_rdlock", "pthread_rwlock_unlock", "pthread_rwlock_wrlock")));
  const bool requires_bionic_pthread = requires_bionic_sync;
  if (requires_bionic_sync && pthread_bridge_handle) {
    // Bridge exports bionic_ names; mutex/cond are default, rwlock opt-in; TLS/attrs stay on the provider.
    const bool bridge_symbol = std::strncmp(symbol, "pthread_mutex", 13) == 0 || std::strncmp(symbol, "pthread_cond", 12) == 0 || std::strncmp(symbol, "pthread_condattr", 15) == 0 ||
                               (bridge_rwlock && std::strncmp(symbol, "pthread_rwlock", 14) == 0);
    if (bridge_symbol) {
      const std::string bridged_name = std::string("bionic_") + symbol;
      if (void* resolved = ::dlsym(pthread_bridge_handle, bridged_name.c_str())) { return traced_provider("pthread-bio", resolved); }
      if (trace_provider) { std::fprintf(stderr, "nuah provider: pthread bridge missing %s for %s\n", bridged_name.c_str(), symbol); }
    }
  }
  const bool requires_bionic_provider = streq_any(symbol, "abort", "fflush", "fread", "fwrite", "fclose", "__sF", "stdin", "stdout", "stderr") || requires_bionic_pthread;
  // Finite Android-only helpers absent from glibc/libhybris; shim stays LOCAL, libhybris owns libc/pthread/TLS.
  const bool requires_bionic_helper = streq_any(symbol, "__FD_CLR_chk", "__FD_ISSET_chk", "__FD_SET_chk", "__assert2", "__fwrite_chk", "__sendto_chk", "__stack_chk_guard", "__strchr_chk",
                                                "__strlen_chk", "__strncpy_chk2", "__system_property_get", "__write_chk", "android_set_abort_message");
  if (bionic_provider_handle && requires_bionic_helper) {
    // Accept unversioned or LIBC-shaped helper DSO; requiring LIBC alone broke real fortify imports.
    if (void* resolved = ::dlvsym(bionic_provider_handle, symbol, "LIBC")) { return resolved; }
    if (void* resolved = ::dlsym(bionic_provider_handle, symbol)) { return resolved; }
  }
  if (bionic_provider_handle && requires_bionic_provider) {
    if (void* resolved = ::dlvsym(bionic_provider_handle, symbol, "LIBC")) { return resolved; }
  }
  // Bionic sysconf numbers differ from glibc; narrow translation here, not another libc.
  if (std::strcmp(symbol, "sysconf") == 0) return reinterpret_cast<void*>(android_sysconf);
  if (is_pthread_symbol) {
    // Never RTLD_DEFAULT: the LOCAL helper's pthread trampolines would fork a second state machine and recurse in TLS init.
    if (host_libc_handle) {
      if (void* resolved = ::dlsym(host_libc_handle, symbol)) { return traced_provider("host-libc", resolved); }
    }
    if (void* resolved = ::dlsym(RTLD_NEXT, symbol)) { return traced_provider("rtld-next", resolved); }
  }
  for (auto it = host_provider_handles.rbegin(); it != host_provider_handles.rend(); ++it) {
    void* resolved = ::dlsym(*it, symbol);
    if (!resolved) continue;
    Dl_info owner{};
    link_map* provider = nullptr;
    if (::dladdr(resolved, &owner) != 0 && ::dlinfo(*it, RTLD_DI_LINKMAP, &provider) == 0 && provider && owner.dli_fbase == reinterpret_cast<void*>(provider->l_addr)) {
      return traced_provider("host-provider", resolved);
    }
  }
  // Ordinary libc/libm/pthread imports use the one host domain; keeps preflight aligned with the linker.
  if (void* resolved = ::dlsym(RTLD_DEFAULT, symbol)) return traced_provider("rtld-default", resolved);
  // Callback miss alone is not a loader failure.
  bootstrap_trace("nuah bootstrap: provider miss symbol=%s requester=%s\n", symbol, requester ? requester : "(unknown)");
  return nullptr;
}
void load_host_provider(const std::filesystem::path& path) {
  // Nuah libandroid.so stays LOCAL (SDL façade vs ATL GTK) to avoid ELF interposition; hybris uses the explicit handle.
  const auto filename = path.filename();
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | (filename == "libandroid.so" ? RTLD_LOCAL : RTLD_GLOBAL));
  if (!handle) throw_dlerror("cannot load Nuah host provider " + path.string());
  if (filename == "libbionic.so") bionic_provider_handle = handle;
  if (filename == "libandroid.so") android_provider_handle = handle;
  const auto callbacks = nuah_bootstrap_diagnostics_callbacks();
  for (const char* setter_name : {"nuah_bionic_set_diagnostics_callbacks", "nuah_log_set_diagnostics_callbacks"})
    if (auto setter = reinterpret_cast<void (*)(const NuahDiagnosticsCallbacks*)>(::dlsym(handle, setter_name))) setter(&callbacks);
  host_provider_handles.push_back(handle);
}
void configure_host_provider_hooks(void* hybris) {
  static bool configured = false;
  if (configured) return;
  const auto android = runtime_directory() / "android";
  // One TLS domain via libhybris host hooks; Nuah providers resolve first, host libc last.
  static constexpr const char* kProviders[] = {"liblog.so", "libandroid.so", "libvulkan.so"};
  for (const auto* name : kProviders) load_host_provider(android / name);
  // Legacy helper imports resolve via RTLD_DEFAULT; helper stays visible without replacing libc/TLS ownership.
  void* helper_handle = ::dlopen((android / "libbionic-linker-helpers.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!helper_handle) throw_dlerror("cannot load Android linker helper provider");
  host_provider_handles.push_back(helper_handle);
  bionic_provider_handle = ::dlopen((android / "libbionic.so").c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!bionic_provider_handle) throw_dlerror("cannot load Nuah Android helper provider");
  const auto callbacks = nuah_bootstrap_diagnostics_callbacks();
  if (auto setter = reinterpret_cast<void (*)(const NuahDiagnosticsCallbacks*)>(::dlsym(bionic_provider_handle, "nuah_bionic_set_diagnostics_callbacks"))) setter(&callbacks);
  // Prefer the pointer-tagged bridge when present; missing binary keeps the table adapter.
  if (auto* pthread_sync_mode = getenv_value("NUAH_PTHREAD_SYNC"); !pthread_sync_mode || std::strcmp(pthread_sync_mode, "bridge") == 0) {
    const auto try_bridge = [&](const std::filesystem::path& candidate) {
      if (pthread_bridge_handle || !std::filesystem::is_regular_file(candidate)) return;
      pthread_bridge_handle = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (pthread_bridge_handle) bootstrap_trace("nuah sync: pthread bridge=%s\n", candidate.c_str());
    };
    if (auto* configured_bridge = getenv_value("NUAH_PTHREAD_BRIDGE_LIBRARY")) try_bridge(configured_bridge);
    try_bridge(runtime_directory() / "bionic-translation" / "libpthread_bio.so.0");
    if (!pthread_bridge_handle) bootstrap_trace("nuah sync: requested pthread bridge unavailable; using table adapter\n");
  }
  host_libc_handle = ::dlopen("libc.so.6", RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
  if (!host_libc_handle) { throw_dlerror("cannot pin host libc for Android pthread symbols"); }
  for (const auto* name : {"libEGL.so.1", "libGLESv2.so.2"}) {
    void* handle = ::dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) { throw_dlerror(std::string("cannot load host graphics provider ") + name); }
    host_provider_handles.push_back(handle);
  }
  const auto set_hook_callback = reinterpret_cast<void (*)(void* (*)(const char*, const char*))>(::dlsym(hybris, "hybris_set_hook_callback"));
  if (!set_hook_callback) fail("libhybris lacks hook callback support");
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
    if (fd < 0) fail("cannot open APK: " + path.string());
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      fail("cannot stat APK: " + path.string());
    }
    size = static_cast<std::size_t>(st.st_size);
    if (size == 0) return;
    data = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
      ::close(fd);
      fail("cannot mmap APK: " + path.string());
    }
    ::madvise(data, size, MADV_WILLNEED | MADV_SEQUENTIAL);
  }
  void release() noexcept {
    if (data && data != MAP_FAILED && size > 0) ::munmap(data, size);
    if (fd >= 0) ::close(fd);
    data = nullptr;
    size = 0;
    fd = -1;
  }
  ~MappedFile() { release(); }
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept : data(other.data), size(other.size), fd(other.fd) { other.data = nullptr; other.size = 0; other.fd = -1; }
  MappedFile& operator=(MappedFile&& other) noexcept {
    if (this != &other) { release(); new (this) MappedFile(std::move(other)); }
    return *this;
  }
  std::span<const std::byte> span() const noexcept {
    if (!data || data == MAP_FAILED) return {};
    return {reinterpret_cast<const std::byte*>(data), size};
  }
};
inline void zip_need(std::span<const std::byte> b, std::size_t off, std::size_t n) { if (off + n > b.size()) fail("truncated ZIP field"); }
inline std::uint16_t u16(std::span<const std::byte> b, std::size_t off) {
  zip_need(b, off, 2);
  return static_cast<std::uint16_t>(static_cast<unsigned char>(b[off])) | (static_cast<std::uint16_t>(static_cast<unsigned char>(b[off + 1])) << 8);
}
inline std::uint32_t u32(std::span<const std::byte> b, std::size_t off) {
  zip_need(b, off, 4);
  return static_cast<std::uint32_t>(static_cast<unsigned char>(b[off])) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 2])) << 16) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 3])) << 24);
}
std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) fail("cannot open APK: " + path.string());
  const auto n = in.tellg();
  if (n < 0) fail("cannot determine APK size");
  std::vector<std::byte> data(static_cast<std::size_t>(n));
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(data.data()), n)) fail("cannot read APK");
  return data;
}
void validate_elf(const std::vector<std::byte>& data) {
  if (data.size() < sizeof(Elf64_Ehdr)) fail("APK member is too small for ELF64");
  Elf64_Ehdr header{};
  if (!read_elf_header(data, header) || header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64 || header.e_type != ET_DYN) fail("APK member is not an x86-64 PIE/shared ELF");
}
std::string sha256_hex(const std::vector<std::byte>& data) {
  const std::string result = nuah::sha256_hex(data);
  if (result.empty()) fail("cannot calculate SHA-256 checksum");
  return result;
}
std::optional<std::string> patch_manifest_field(const std::string& manifest, const char* field) {
  const std::regex pattern("\\\"" + std::string(field) + "\\\"\\s*:\\s*(?:\\\"([^\\\"]*)\\\"|([0-9]+))");
  std::smatch match;
  if (!std::regex_search(manifest, match, pattern) || match.size() != 3) return std::nullopt;
  return match[1].matched ? match[1].str() : match[2].str();
}
std::optional<std::string> elf_build_id(const std::vector<std::byte>& data) {
  if (data.size() < sizeof(Elf64_Ehdr)) return std::nullopt;
  Elf64_Ehdr header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_phentsize != sizeof(Elf64_Phdr)) return std::nullopt;
  std::vector<Elf64_Phdr> programs;
  if (!copy_program_headers(data, header, programs)) return std::nullopt;
  for (const auto& program : programs) {
    if (program.p_type != PT_NOTE || !elf_in_bounds(data, program.p_offset, program.p_filesz)) continue;
    std::size_t cursor = static_cast<std::size_t>(program.p_offset);
    const std::size_t end = cursor + static_cast<std::size_t>(program.p_filesz);
    while (cursor + 12 <= end) {
      std::uint32_t namesz = 0, descsz = 0, type = 0;
      std::memcpy(&namesz, data.data() + cursor, 4);
      std::memcpy(&descsz, data.data() + cursor + 4, 4);
      std::memcpy(&type, data.data() + cursor + 8, 4);
      cursor += 12;
      const std::size_t name_bytes = (static_cast<std::size_t>(namesz) + 3) & ~3U;
      const std::size_t desc_bytes = (static_cast<std::size_t>(descsz) + 3) & ~3U;
      if (name_bytes > end - cursor || desc_bytes > end - cursor - name_bytes) break;
      if (type == NT_GNU_BUILD_ID && namesz >= 3 && std::memcmp(data.data() + cursor, "GNU", 3) == 0) {
        std::string result;
        result.reserve(descsz * 2);
        static constexpr char digits[] = "0123456789abcdef";
        for (std::size_t byte = 0; byte < descsz; ++byte) {
          const auto value = static_cast<unsigned char>(data[cursor + name_bytes + byte]);
          result.push_back(digits[value >> 4]);
          result.push_back(digits[value & 0xf]);
        }
        return result;
      }
      cursor += name_bytes + desc_bytes;
    }
  }
  return std::nullopt;
}
void validate_libroblox_patch_overlay(const std::filesystem::path& path, const std::vector<std::byte>& original, const std::vector<std::byte>& patched) {
  const auto manifest_bytes = read_file(path.string() + ".json");
  const std::string manifest(reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size());
  const auto field = [&](const char* name) {
    const auto value = patch_manifest_field(manifest, name);
    if (!value) fail("libroblox patch manifest lacks " + std::string(name));
    return *value;
  };
  if (field("format") != "1" || field("patch") != "TexturePackGeneratorUseOriginalDefault" || field("target") != "libroblox.so") fail("unsupported libroblox patch manifest");
  if (field("original_sha256") != sha256_hex(original) || field("patched_sha256") != sha256_hex(patched) || field("original_size") != std::to_string(original.size()) ||
      field("patched_size") != std::to_string(patched.size()))
    fail("libroblox patch manifest does not match its images");
  if (field("original_bytes") != "31c0" || field("replacement_bytes") != "b001") fail("unexpected libroblox patch bytes");
  const auto original_id = elf_build_id(original);
  const auto patched_id = elf_build_id(patched);
  if (!original_id || !patched_id || *original_id != *patched_id || field("build_id") != *original_id) fail("libroblox patch build ID mismatch");
  bootstrap_trace("nuah loader: validated libroblox sidecar %s build=%s\n", path.c_str(), patched_id->c_str());
}
void write_all(int fd, const std::vector<std::byte>& data) {
  for (std::size_t done = 0; done < data.size();) {
    const auto n = ::write(fd, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) fail("temporary ELF write failed");
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
  if (::inflateInit2(&stream, -MAX_WBITS) != Z_OK) fail("cannot initialize APK deflate stream");
  const auto status = ::inflate(&stream, Z_FINISH);
  ::inflateEnd(&stream);
  if (status != Z_STREAM_END || stream.total_out != expected_size || stream.total_in != compressed.size()) fail("invalid or truncated APK deflate member");
  return result;
}
template <typename Visitor>
void visit_apk_members(const std::filesystem::path& apk, Visitor&& visitor) {
  const MappedFile mapped(apk);
  const auto data = mapped.span();
  const std::size_t first = data.size() > 0xffff + 22 ? data.size() - (0xffff + 22) : 0;
  std::size_t eocd = data.size();
  for (std::size_t i = data.size() >= 22 ? data.size() - 22 : 0; i >= first; --i) {
    if (u32(data, i) == kEndOfCentralDirectory) {
      eocd = i;
      break;
    }
    if (i == 0) break;
  }
  if (eocd == data.size()) fail("APK has no ZIP central directory");
  const std::uint16_t entries = u16(data, eocd + 10);
  std::size_t pos = u32(data, eocd + 16);
  for (std::uint16_t i = 0; i < entries; ++i) {
    if (u32(data, pos) != kCentralDirectory) fail("invalid ZIP central directory entry");
    const std::uint16_t method = u16(data, pos + 10);
    const std::uint32_t compressed_size = u32(data, pos + 20), uncompressed_size = u32(data, pos + 24);
    const std::uint16_t name_len = u16(data, pos + 28), extra_len = u16(data, pos + 30), comment_len = u16(data, pos + 32);
    const std::uint32_t local = u32(data, pos + 42);
    const std::size_t next = pos + 46 + name_len + extra_len + comment_len;
    if (next > data.size()) fail("truncated ZIP central directory name");
    if (visitor(std::string(reinterpret_cast<const char*>(data.data() + pos + 46), name_len), method, compressed_size, uncompressed_size, local, data)) return;
    pos = next;
  }
}
std::vector<std::byte> decode_member(std::span<const std::byte> data, std::uint16_t method, std::uint32_t compressed_size, std::uint32_t uncompressed_size, std::uint32_t local) {
  if (u32(data, local) != kLocalFile) fail("invalid ZIP local header");
  const std::size_t start = static_cast<std::size_t>(local) + 30 + u16(data, local + 26) + u16(data, local + 28);
  if (start + compressed_size > data.size()) fail("truncated APK member data");
  std::span<const std::byte> encoded(data.data() + start, compressed_size);
  if (method == 0) {
    if (compressed_size != uncompressed_size) fail("stored APK member has inconsistent size");
    return std::vector<std::byte>(encoded.begin(), encoded.end());
  }
  if (method == 8) return inflate_raw(encoded, uncompressed_size);
  fail("unsupported APK ZIP compression method");
}
// Shared C-string extraction for DT_NEEDED/DYNSYM scans.
inline std::string elf_cstring_range(const char* begin, const char* end, const char* what) {
  const auto* nul = static_cast<const char*>(std::memchr(begin, '\0', end - begin));
  if (!nul) fail(what);
  return std::string(begin, nul);
}
std::vector<std::string> needed_libraries(const std::vector<std::byte>& data) {
  validate_elf(data);
  Elf64_Ehdr header{};
  std::memcpy(&header, data.data(), sizeof(header));
  std::vector<Elf64_Phdr> programs;
  if (!copy_program_headers(data, header, programs)) fail("ELF program headers are outside the image");
  const Elf64_Phdr* dynamic = find_dynamic_segment(programs.data(), header.e_phnum);
  if (!dynamic) return {};
  if (!elf_in_bounds(data, dynamic->p_offset, dynamic->p_filesz) || dynamic->p_filesz % sizeof(Elf64_Dyn)) fail("ELF dynamic section is outside the image");
  const auto* entries = reinterpret_cast<const Elf64_Dyn*>(data.data() + dynamic->p_offset);
  const std::size_t count = dynamic->p_filesz / sizeof(Elf64_Dyn);
  Elf64_Xword strtab_address = 0;
  std::vector<Elf64_Xword> needed_offsets;
  for (std::size_t i = 0; i < count && entries[i].d_tag != DT_NULL; ++i) {
    if (entries[i].d_tag == DT_STRTAB) strtab_address = entries[i].d_un.d_ptr;
    if (entries[i].d_tag == DT_NEEDED) needed_offsets.push_back(entries[i].d_un.d_val);
  }
  if (!strtab_address) return {};
  std::uint64_t strtab_offset = 0;
  if (!va_to_file_offset(programs.data(), header.e_phnum, data, strtab_address, 1, &strtab_offset)) fail("ELF string table is outside the image");
  std::vector<std::string> result;
  for (const auto offset : needed_offsets) {
    const auto at = strtab_offset + static_cast<std::size_t>(offset);
    if (at >= data.size()) fail("ELF dependency name is outside the image");
    result.push_back(elf_cstring_range(reinterpret_cast<const char*>(data.data() + at), reinterpret_cast<const char*>(data.data() + data.size()), "unterminated ELF dependency name"));
  }
  return result;
}
std::vector<std::string> undefined_dynamic_symbols(const std::vector<std::byte>& data) {
  validate_elf(data);
  Elf64_Ehdr header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (!header.e_shoff || !header.e_shnum || !elf_in_bounds(data, header.e_shoff, static_cast<std::uint64_t>(header.e_shnum) * sizeof(Elf64_Shdr))) fail("ELF section headers are outside the image");
  std::vector<std::string> result;
  for (std::uint16_t index = 0; index < header.e_shnum; ++index) {
    Elf64_Shdr symbols{}, strings{};
    std::memcpy(&symbols, data.data() + header.e_shoff + index * sizeof(Elf64_Shdr), sizeof(symbols));
    if (symbols.sh_type != SHT_DYNSYM || symbols.sh_entsize != sizeof(Elf64_Sym) || symbols.sh_link >= header.e_shnum || !elf_in_bounds(data, symbols.sh_offset, symbols.sh_size)) continue;
    std::memcpy(&strings, data.data() + header.e_shoff + symbols.sh_link * sizeof(Elf64_Shdr), sizeof(strings));
    if (strings.sh_type != SHT_STRTAB || !elf_in_bounds(data, strings.sh_offset, strings.sh_size)) fail("ELF dynamic string table is outside the image");
    const auto count = symbols.sh_size / sizeof(Elf64_Sym);
    for (std::size_t symbol_index = 0; symbol_index < count; ++symbol_index) {
      Elf64_Sym symbol{};
      std::memcpy(&symbol, data.data() + symbols.sh_offset + symbol_index * sizeof(Elf64_Sym), sizeof(symbol));
      if (symbol.st_shndx != SHN_UNDEF || ELF64_ST_BIND(symbol.st_info) != STB_GLOBAL || !symbol.st_name || symbol.st_name >= strings.sh_size) continue;
      result.push_back(elf_cstring_range(reinterpret_cast<const char*>(data.data() + strings.sh_offset + symbol.st_name),
                                         reinterpret_cast<const char*>(data.data() + strings.sh_offset + strings.sh_size), "unterminated ELF dynamic symbol name"));
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}
void preflight_host_hooks(const std::vector<std::byte>& image, const char* requester) {
  std::vector<std::string> missing;
  for (const auto& symbol : undefined_dynamic_symbols(image))
    if (!resolve_host_provider_symbol(symbol.c_str(), requester)) missing.push_back(symbol);
  if (missing.empty()) return;
  std::string message = "libhybris preflight found " + std::to_string(missing.size()) + " unresolved strong symbol" + (missing.size() != 1 ? "s:" : ":");
  for (const auto& symbol : missing) message += " " + symbol;
  fail(message);
}
}  // namespace
ApkMember read_stored_apk_member(const std::filesystem::path& apk, const std::string& member) {
  std::optional<ApkMember> result;
  visit_apk_members(apk, [&](const std::string& name, std::uint16_t method, std::uint32_t compressed, std::uint32_t uncompressed, std::uint32_t local, std::span<const std::byte> data) {
    if (name != member) return false;
    result = ApkMember{name, decode_member(data, method, compressed, uncompressed, local)};
    return true;
  });
  if (!result) fail("APK member not found: " + member);
  return std::move(*result);
}
std::vector<ApkMember> read_apk_members_with_prefix(const std::filesystem::path& apk, const std::string& prefix) {
  std::vector<ApkMember> result;
  visit_apk_members(apk, [&](const std::string& name, std::uint16_t method, std::uint32_t compressed, std::uint32_t uncompressed, std::uint32_t local, std::span<const std::byte> data) {
    if (name.starts_with(prefix) && !name.ends_with("/")) result.push_back(ApkMember{name, decode_member(data, method, compressed, uncompressed, local)});
    return false;
  });
  return result;
}
std::vector<std::string> elf_needed_libraries(const std::vector<std::byte>& elf_bytes) { return needed_libraries(elf_bytes); }
void prepend_search_path(const char* name, const std::string& piece) {
  if (piece.empty()) return;
  auto* existing = getenv_value(name);
  if (existing && std::string(existing).find(piece) != std::string::npos) return;
  std::string value = piece;
  if (existing) {
    value.push_back(':');
    value += existing;
  }
  if (::setenv(name, value.c_str(), 1) != 0) fail(std::string("cannot configure ") + name);
}
template <typename F>
inline F dlsym_fn(void* handle, const char* name) { return reinterpret_cast<F>(::dlsym(handle, name)); }
void configure_android_library_path(const std::filesystem::path& app_directory) {
  const std::string app_library = (app_directory / "lib").string();
  const std::string app_tree = app_directory.string() + "**";
  for (const char* env : {"HYBRIS_LD_LIBRARY_PATH", "BIONIC_LD_LIBRARY_PATH"}) {
    prepend_search_path(env, app_library);
    prepend_search_path(env, app_tree);
  }
  const char* configured_path = getenv_value("BIONIC_LD_LIBRARY_PATH");
  std::string path = app_library + (configured_path ? ":" + std::string(configured_path) : "") + ":" + app_tree;
  using ParseLibraryPath = void (*)(const char*, char*);
  void* linker_handle = bionic_provider_handle;
  ParseLibraryPath parse = linker_handle ? dlsym_fn<ParseLibraryPath>(linker_handle, "dl_parse_library_path") : nullptr;
  if (!parse) parse = dlsym_fn<ParseLibraryPath>(RTLD_DEFAULT, "dl_parse_library_path");
  if (!parse) {
    static const char* candidates[] = {"/lib64/libdl_bio.so.0", "/usr/lib64/libdl_bio.so.0", "/lib/libdl_bio.so.0", "/usr/lib/libdl_bio.so.0"};
    for (const char* candidate : candidates) {
      void* linker = ::dlopen(candidate, RTLD_NOW | RTLD_GLOBAL);
      if (!linker) continue;
      linker_handle = linker;
      parse = dlsym_fn<ParseLibraryPath>(linker, "dl_parse_library_path");
      if (parse) break;
      ::dlclose(linker);
    }
  }
  if (parse) {
    char delimiter[] = ":";
    parse(path.data(), delimiter);
  }
  auto* linker_debug = linker_handle ? dlsym_fn<struct r_debug**>(linker_handle, "_r_debug_ptr") : nullptr;
  if (!linker_debug) linker_debug = dlsym_fn<struct r_debug**>(RTLD_DEFAULT, "_r_debug_ptr");
  if (linker_debug && !*linker_debug)
    if (auto* host_debug = dlsym_fn<struct r_debug*>(RTLD_DEFAULT, "_r_debug")) *linker_debug = host_debug;
}
LoadedModule::~LoadedModule() {
  // App images stay resident (Android model); unloading runs Roblox destructors after the FILE domain changed.
  const auto filename = path_.filename().string();
  const bool app_library_lifetime = filename == "libroblox.so" || filename.starts_with("libroblox.");
  if (!app_library_lifetime && handle_ && close_) close_(handle_);
  if (!app_library_lifetime && loader_library_) ::dlclose(loader_library_);
  if (remove_path_ && !path_.empty()) {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
}
LoadedModule::LoadedModule(LoadedModule&& other) noexcept
    : path_(std::move(other.path_)),
      handle_(std::exchange(other.handle_, nullptr)),
      loader_library_(std::exchange(other.loader_library_, nullptr)),
      close_(std::exchange(other.close_, nullptr)),
      symbol_(std::exchange(other.symbol_, nullptr)),
      versioned_symbol_(std::exchange(other.versioned_symbol_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      remove_path_(other.remove_path_) {
  other.path_.clear();
  other.remove_path_ = true;
}
LoadedModule& LoadedModule::operator=(LoadedModule&& other) noexcept {
  if (this != &other) { this->~LoadedModule(); new (this) LoadedModule(std::move(other)); }
  return *this;
}
// Isolated child uses _exit(), so reap only our nuah-module-* files on the next launch.
void reap_stale_native_modules(const std::filesystem::path& directory) {
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error) return;
  std::size_t removed = 0;
  std::uintmax_t removed_bytes = 0;
  for (std::filesystem::directory_iterator entries(directory, std::filesystem::directory_options::skip_permission_denied, error); entries != std::filesystem::directory_iterator(); entries.increment(error)) {
    if (error) { error.clear(); continue; }
    if (!entries->path().filename().string().starts_with("nuah-module-")) continue;
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(entries->path(), file_error) || file_error) continue;
    const auto bytes = std::filesystem::file_size(entries->path(), file_error);
    if (file_error) continue;
    if (std::filesystem::remove(entries->path(), file_error) && !file_error) {
      ++removed;
      removed_bytes += bytes;
    }
  }
  if (removed) bootstrap_trace("nuah loader: reaped %zu stale native modules (%llu MiB)\n", removed, static_cast<unsigned long long>(removed_bytes / (1024 * 1024)));
}
void* LoadedModule::symbol(const char* name) const {
  if (!handle_ || !name) return nullptr;
  if (symbol_)
    if (void* result = symbol_(handle_, name)) return result;
  // Toolchain exports JNI entries under LIBROBLOX; plain dlsym misses them, so keep the versioned fallback here.
  return versioned_symbol_ ? versioned_symbol_(handle_, name, "LIBROBLOX") : nullptr;
}
LoadedModule load_apk_library(const std::filesystem::path& apk, const std::string& member) {
  const auto apk_member = read_stored_apk_member(apk, member);
  validate_elf(apk_member.bytes);
  auto* patch_setting = getenv_value("NUAH_LIBROBLOX_PATCH");
  const bool memory_patch_requested = (patch_setting && streq_any(patch_setting, "memory", "in-memory")) || env_flag("NUAH_LIBROBLOX_MEMORY_PATCH");
  std::vector<std::byte> overlay_bytes;
  const std::filesystem::path overlay_path = [&] {
    if (!patch_setting || streq_any(patch_setting, "memory", "in-memory")) return std::filesystem::path{};
    const std::filesystem::path candidate(patch_setting);
    overlay_bytes = read_file(candidate);
    validate_elf(overlay_bytes);
    validate_libroblox_patch_overlay(candidate, apk_member.bytes, overlay_bytes);
    return candidate;
  }();
  const auto& image_bytes = overlay_path.empty() ? apk_member.bytes : overlay_bytes;
  // Reuse native_runtime's app-private extraction when present; temp ELF is the fallback.
  auto* app_data = getenv_value("ANDROID_APP_DATA_DIR");
  std::filesystem::path staging_root = app_data ? std::filesystem::path(app_data) / ".native-tmp" : std::filesystem::path("/tmp");
  std::error_code staging_error;
  std::filesystem::create_directories(staging_root, staging_error);
  if (staging_error) fail("cannot create native staging directory: " + staging_error.message());
  reap_stale_native_modules(staging_root);
  std::filesystem::path path;
  std::filesystem::path temporary_path;
  bool remove_path = true;
  if (!overlay_path.empty()) {
    path = overlay_path;
    remove_path = false;
    bootstrap_trace("nuah loader: using libroblox sidecar %s\n", path.c_str());
  } else if (app_data) {
    const auto app_path = std::filesystem::path(app_data) / "lib" / std::filesystem::path(member).filename();
    std::error_code app_stat;
    if (std::filesystem::is_regular_file(app_path, app_stat) && std::filesystem::file_size(app_path, app_stat) == image_bytes.size()) {
      path = app_path;
      remove_path = false;
      bootstrap_trace("nuah loader: reusing extracted native image %s\n", path.c_str());
    }
  }
  int fd = -1;
  try {
    if (path.empty()) {
      const std::string pattern = (staging_root / "nuah-module-XXXXXX").string();
      std::vector<char> path_template(pattern.begin(), pattern.end());
      path_template.push_back('\0');
      fd = ::mkstemp(path_template.data());
      if (fd < 0) fail("temporary ELF file creation failed");
      temporary_path = std::filesystem::path(path_template.data());
      path = temporary_path;
      write_all(fd, image_bytes);
      if (::fchmod(fd, 0500) != 0) fail("temporary ELF permission setup failed");
      if (::close(fd) != 0) fail("temporary ELF close failed");
      fd = -1;
    }
    const std::string library = hybris_common_library().string();
    configure_hybris_environment(library.c_str());
    if (app_data) configure_android_library_path(app_data);
    void* loader_library = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!loader_library) throw_dlerror("cannot load libhybris common library");
    const auto android_dlopen = dlsym_fn<void* (*)(const char*, int)>(loader_library, "android_dlopen");
    const auto android_dlerror = dlsym_fn<char* (*)()>(loader_library, "android_dlerror");
    const auto android_dlclose = dlsym_fn<int (*)(void*)>(loader_library, "android_dlclose");
    const auto android_dlsym = dlsym_fn<void* (*)(void*, const char*)>(loader_library, "android_dlsym");
    const auto android_dlvsym = dlsym_fn<void* (*)(void*, const char*, const char*)>(loader_library, "android_dlvsym");
    if (!android_dlopen || !android_dlerror || !android_dlclose || !android_dlsym) {
      ::dlclose(loader_library);
      fail("libhybris common library lacks Android loader entrypoints");
    }
    configure_host_provider_hooks(loader_library);
    preflight_host_hooks(image_bytes, path.c_str());
    if (bootstrap_trace_enabled()) {
      void* libc_handle = android_dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
      if (libc_handle) {
        trace_sdk_property("Android libc property", android_dlsym(libc_handle, "__system_property_get"));
        (void)android_dlclose(libc_handle);
      } else {
        std::fprintf(stderr, "nuah loader: Android libc lookup failed: %s\n", android_dlerror ? android_dlerror() : "unknown");
      }
    }
    // Prefer the installed bionic loader so ATL shares one handle; libhybris is the fallback.
    using BionicDlopen = void* (*)(const char*, int);
    using BionicDlerror = const char* (*)();
    using BionicDlclose = int (*)(void*);
    using BionicDlsym = void* (*)(void*, const char*);
    const auto bionic_dlopen = dlsym_fn<BionicDlopen>(RTLD_DEFAULT, "bionic_dlopen");
    const auto bionic_dlerror = dlsym_fn<BionicDlerror>(RTLD_DEFAULT, "bionic_dlerror");
    const auto bionic_dlclose = dlsym_fn<BionicDlclose>(RTLD_DEFAULT, "bionic_dlclose");
    const auto bionic_dlsym = dlsym_fn<BionicDlsym>(RTLD_DEFAULT, "bionic_dlsym");
    bool bionic_handle = false;
    void* handle = nullptr;
    if (bionic_dlopen && bionic_dlclose && bionic_dlsym) {
      // Promote LOCAL libandroid for try_glibc soname fallback; never pass host ELF to bionic_dlopen (rejects IFUNC/TLS).
      const auto host_android = runtime_directory() / "android" / "libandroid.so";
      if (std::filesystem::is_regular_file(host_android)) {
        void* promoted = ::dlopen(host_android.c_str(), RTLD_NOW | RTLD_GLOBAL);
        bootstrap_trace("nuah loader: promoted host libandroid handle=%p path=%s\n", promoted, host_android.c_str());
      }
      handle = bionic_dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      bionic_handle = handle != nullptr;
      if (bootstrap_trace_enabled()) {
        const char* error = bionic_dlerror ? bionic_dlerror() : nullptr;
        std::fprintf(stderr, "nuah loader: bionic_dlopen(%s) handle=%p error=%s\n", path.c_str(), handle, error ? error : "none");
      }
    }
    if (!handle) {
      handle = android_dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (bootstrap_trace_enabled()) std::fprintf(stderr, "nuah loader: android_dlopen(%s) handle=%p error=%s\n", path.c_str(), handle, android_dlerror ? android_dlerror() : "unknown");
    }
    if (!handle) {
      std::string message = "android_dlopen failed: ";
      const char* error = bionic_handle ? (bionic_dlerror ? bionic_dlerror() : nullptr) : android_dlerror();
      message += error ? error : "unknown loader error";
      const auto needed = needed_libraries(apk_member.bytes);
      if (!needed.empty()) {
        message += " (Android image needs";
        for (const auto& library : needed) message += " " + library;
        message += "; libhybris host hooks could not resolve the image)";
      }
      if (loader_library) ::dlclose(loader_library);
      fail(message);
    }
    LoadedModule result;
    result.path_ = path;
    result.handle_ = handle;
    result.loader_library_ = loader_library;
    result.close_ = bionic_handle ? bionic_dlclose : android_dlclose;
    result.symbol_ = bionic_handle ? bionic_dlsym : android_dlsym;
    result.versioned_symbol_ = bionic_handle ? nullptr : android_dlvsym;
    result.size_ = image_bytes.size();
    result.remove_path_ = remove_path;
    // Post-relocation in-memory A/B only; no APK/sidecar bytes change, page returns to RX.
    if (memory_patch_requested && !patch_loaded_module_texture_flag_impl(result)) fail("requested in-memory libroblox texture patch could not be applied");
    if (bootstrap_trace_enabled()) {
      const auto lookup = bionic_handle ? bionic_dlsym : android_dlsym;
      if (lookup) {
        if (void* property = lookup(handle, "__system_property_get")) trace_sdk_property("libroblox __system_property_get", property);
        else std::fprintf(stderr, "nuah loader: libroblox __system_property_get unresolved\n");
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
bool patch_loaded_module_property_import(const LoadedModule& module) { return patch_loaded_module_property_import_impl(module); }
bool patch_loaded_module_texture_flag(const LoadedModule& module) { return patch_loaded_module_texture_flag_impl(module); }
namespace {
void prepend_path_env(const char* name, const std::filesystem::path& item) {
  if (item.empty()) return;
  std::error_code error;
  if (!std::filesystem::exists(item, error) || error) return;
  prepend_search_path(name, item.string());
}
}  // namespace
void apply_native_host_environment() {
  const auto hybris = hybris_common_library();
  std::error_code hybris_error;
  if (!std::filesystem::exists(hybris, hybris_error) || hybris_error) fail("libhybris is missing at " + hybris.string() + " (expected beside the binary as hybris/lib/libhybris-common.so; set NUAH_HYBRIS_LIBRARY to override)");
  set_hybris_path_if_unset("NUAH_HYBRIS_LIBRARY", hybris.string());
  configure_hybris_environment(hybris.c_str());
  prepend_path_env("LD_LIBRARY_PATH", hybris.parent_path());
  std::error_code real_error;
  if (const auto real = std::filesystem::canonical(hybris, real_error); !real_error && real != hybris) std::cerr << "nuah runtime: hybris=" << hybris << " -> " << real << '\n';
  else std::cerr << "nuah runtime: hybris=" << hybris << '\n';
  prepend_path_env("LD_LIBRARY_PATH", runtime_directory());
  std::filesystem::path art_library = "/usr/local/lib64/art";
  if (auto* configured = getenv_value("NUAH_ART_LIBRARY")) art_library = std::filesystem::path(configured).parent_path();
  else if (auto* configured = getenv_value("NUAH_ART_LIBRARY_DIR")) art_library = configured;
  for (const auto& item : {art_library, art_library / "natives"}) prepend_path_env("LD_LIBRARY_PATH", item);
  for (const char* soname : {"libandroidfw.so", "libicudata.so.77", "libicuuc.so.77", "libicui18n.so.77"}) prepend_path_env("LD_PRELOAD", art_library / soname);
  for (const char* system : {"/usr/lib64/libpng16.so.16", "/usr/lib64/libjpeg.so.62"}) prepend_path_env("LD_PRELOAD", system);
  constexpr const char* kArtHome = "/usr/local/lib64/java/dex/android_translation_layer";
  static constexpr std::pair<const char*, const char*> kDefaults[] = {
      {"NUAH_ART_HOME", kArtHome}, {"NUAH_GRAPHICS_BACKEND", "vulkan"}, {"NUAH_VULKAN_PRESENT_MODE", "fifo"},
      {"NUAH_CLIENT_SETTINGS_JSON", "{\"applicationSettings\":{\"DFFlagDebugDisableRbxTransportDummyClient\":true}}"},
  };
  // Range loop keeps the four defaults from drifting apart (table needs runtime std::string construction).
  for (const auto& [name, value] : kDefaults) set_hybris_path_if_unset(name, value);
}
}  // namespace nuah
