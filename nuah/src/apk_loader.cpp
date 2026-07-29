#include "nuah/apk_loader.hpp"
#include "nuah/bootstrap_diagnostics.h"

#include <elf.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
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

void configure_hybris_environment(const char* library) {
  // Only Nuah's narrow Android-facing providers belong on the linker path.
  // In particular, do not accept a bionic/APEX directory here: libhybris must
  // resolve the libc/libdl/libm ABI boundary through its host hooks.
  set_hybris_path_if_unset("HYBRIS_LD_LIBRARY_PATH", (runtime_directory() / "android").string());
  if (!library || !*library || ::getenv("HYBRIS_LINKER_DIR")) return;
  const std::filesystem::path common(library);
  if (!common.has_parent_path()) return;
  set_hybris_path_if_unset(
      "HYBRIS_LINKER_DIR",
      (common.parent_path() / "libhybris" / "linker").string());
}

std::vector<void*> host_provider_handles;
void* bionic_provider_handle = nullptr;
using HybrisBuiltinHook = void* (*)(const char*, const char*);
HybrisBuiltinHook hybris_builtin_hook = nullptr;
std::uintptr_t host_stack_chk_guard = 0x9e3779b97f4a7c15ULL;

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
  // A small audited set must precede libhybris: these objects have different
  // API-36 x86-64 layouts (pthread attributes and legacy Bionic FILE), or
  // carry Nuah's constructor diagnostics. Everything else delegates to
  // libhybris before the finite provider fallback.
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
      std::strcmp(symbol, "pthread_attr_init") == 0 ||
      std::strcmp(symbol, "pthread_attr_destroy") == 0 ||
      std::strcmp(symbol, "pthread_attr_getstack") == 0 ||
      std::strcmp(symbol, "pthread_attr_setdetachstate") == 0 ||
      std::strcmp(symbol, "pthread_attr_setschedparam") == 0 ||
      std::strcmp(symbol, "pthread_attr_setstacksize") == 0 ||
      std::strcmp(symbol, "pthread_create") == 0 ||
      std::strcmp(symbol, "pthread_getattr_np") == 0;
  if (bionic_provider_handle && requires_bionic_provider) {
    if (void* resolved =
            ::dlvsym(bionic_provider_handle, symbol, "LIBC")) {
      return resolved;
    }
  }
  if (std::strcmp(symbol, "sysconf") == 0) {
    return reinterpret_cast<void*>(android_sysconf);
  }
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
  const bool requires_hybris_synchronization =
      std::strncmp(symbol, "pthread_mutex", 13) == 0 ||
      std::strncmp(symbol, "pthread_cond", 12) == 0 ||
      std::strncmp(symbol, "pthread_rwlock", 14) == 0 ||
      std::strcmp(symbol, "pthread_once") == 0;
  if (requires_hybris_synchronization && hybris_builtin_hook) {
    return hybris_builtin_hook(symbol, requester);
  }
  for (auto it = host_provider_handles.rbegin(); it != host_provider_handles.rend(); ++it) {
    void* resolved = ::dlsym(*it, symbol);
    if (!resolved) continue;
    Dl_info owner{};
    link_map* provider = nullptr;
    if (::dladdr(resolved, &owner) != 0 &&
        ::dlinfo(*it, RTLD_DI_LINKMAP, &provider) == 0 && provider &&
        owner.dli_fbase == reinterpret_cast<void*>(provider->l_addr)) {
      return resolved;
    }
  }
  if (hybris_builtin_hook) {
    if (void* resolved = hybris_builtin_hook(symbol, requester)) {
      return resolved;
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
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (handle) {
    if (path.filename() == "libbionic.so") bionic_provider_handle = handle;
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
  for (const auto* name : {"libbionic.so", "libm.so", "liblog.so", "libandroid.so", "libvulkan.so", "libmediandk.so",
                           "libOpenSLES.so", "libOpenMAXAL.so"}) {
    load_host_provider(android / name);
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

std::uint16_t u16(const std::vector<std::byte>& b, std::size_t off) {
  if (off + 2 > b.size()) throw std::runtime_error("truncated ZIP field");
  return static_cast<std::uint16_t>(static_cast<unsigned char>(b[off])) |
         (static_cast<std::uint16_t>(static_cast<unsigned char>(b[off + 1])) << 8);
}
std::uint32_t u32(const std::vector<std::byte>& b, std::size_t off) {
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

void write_all(int fd, const std::vector<std::byte>& data) {
  std::size_t done = 0;
  while (done < data.size()) {
    const auto n = ::write(fd, data.data() + done, data.size() - done);
    if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("temporary ELF write failed"); }
    done += static_cast<std::size_t>(n);
  }
}

std::vector<std::byte> inflate_raw(const std::vector<std::byte>& compressed, std::size_t expected_size) {
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
  const auto data = read_file(apk);
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

std::vector<std::byte> decode_member(const std::vector<std::byte>& data,
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
  const std::vector<std::byte> encoded(data.begin() + start,
                                       data.begin() + start + compressed_size);
  if (method == 0) {
    if (compressed_size != uncompressed_size)
      throw std::runtime_error("stored APK member has inconsistent size");
    return encoded;
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
               std::uint32_t local, const std::vector<std::byte>& data) {
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
               std::uint32_t local, const std::vector<std::byte>& data) {
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

LoadedModule::~LoadedModule() {
  if (handle_ && close_) close_(handle_);
  if (loader_library_) ::dlclose(loader_library_);
  if (!path_.empty()) {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
}
LoadedModule::LoadedModule(LoadedModule&& other) noexcept : path_(std::move(other.path_)), handle_(other.handle_), loader_library_(other.loader_library_), close_(other.close_), symbol_(other.symbol_), size_(other.size_) {
  other.path_.clear(); other.handle_ = nullptr; other.loader_library_ = nullptr; other.close_ = nullptr; other.symbol_ = nullptr; other.size_ = 0;
}
LoadedModule& LoadedModule::operator=(LoadedModule&& other) noexcept {
  if (this != &other) { this->~LoadedModule(); path_ = std::move(other.path_); handle_ = other.handle_; loader_library_ = other.loader_library_; close_ = other.close_; symbol_ = other.symbol_; size_ = other.size_; other.path_.clear(); other.handle_ = nullptr; other.loader_library_ = nullptr; other.close_ = nullptr; other.symbol_ = nullptr; other.size_ = 0; }
  return *this;
}

void* LoadedModule::symbol(const char* name) const {
  return handle_ && symbol_ ? symbol_(handle_, name) : nullptr;
}

LoadedModule load_apk_library(const std::filesystem::path& apk, const std::string& member) {
  const auto apk_member = read_stored_apk_member(apk, member);
  validate_elf(apk_member.bytes);
  const auto& image_bytes = apk_member.bytes;
  char path_template[] = "/tmp/nuah-module-XXXXXX";
  int fd = ::mkstemp(path_template);
  if (fd < 0) throw std::runtime_error("temporary ELF file creation failed");
  const std::filesystem::path path(path_template);
  try {
    write_all(fd, image_bytes);
    if (::fchmod(fd, 0500) != 0) throw std::runtime_error("temporary ELF permission setup failed");
    if (::close(fd) != 0) throw std::runtime_error("temporary ELF close failed");
    fd = -1;
    void* loader_library = nullptr;
    const char* configured_library = ::getenv("NUAH_HYBRIS_LIBRARY");
    const std::string library = configured_library && *configured_library
        ? configured_library
        : (runtime_directory() / "hybris" / "lib" / "libhybris-common.so").string();
    configure_hybris_environment(library.c_str());
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
    if (!android_dlopen || !android_dlerror || !android_dlclose || !android_dlsym) {
      ::dlclose(loader_library);
      throw std::runtime_error("libhybris common library lacks Android loader entrypoints");
    }
    configure_host_provider_hooks(loader_library);
    preflight_host_hooks(image_bytes, path.c_str());
    void* handle = android_dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      std::string message = "android_dlopen failed: ";
      const char* error = android_dlerror();
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
    LoadedModule result; result.path_ = path; result.handle_ = handle; result.loader_library_ = loader_library; result.close_ = android_dlclose; result.symbol_ = android_dlsym; result.size_ = image_bytes.size();
    return result;
  } catch (...) {
    if (fd >= 0) ::close(fd);
    std::error_code error;
    std::filesystem::remove(path, error);
    throw;
  }
}
}  // namespace nuah
