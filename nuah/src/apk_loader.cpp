#include "nuah/apk_loader.hpp"

#include <elf.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <link.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <zlib.h>

#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace nuah {
namespace {
constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kCentralDirectory = 0x02014b50;
constexpr std::uint32_t kLocalFile = 0x04034b50;

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
    if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("memfd write failed"); }
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

LoadedModule::~LoadedModule() {
  if (handle_) ::dlclose(handle_);
  if (fd_ >= 0) ::close(fd_);
}
LoadedModule::LoadedModule(LoadedModule&& other) noexcept : fd_(other.fd_), handle_(other.handle_), size_(other.size_) {
  other.fd_ = -1; other.handle_ = nullptr; other.size_ = 0;
}
LoadedModule& LoadedModule::operator=(LoadedModule&& other) noexcept {
  if (this != &other) { this->~LoadedModule(); fd_ = other.fd_; handle_ = other.handle_; size_ = other.size_; other.fd_ = -1; other.handle_ = nullptr; other.size_ = 0; }
  return *this;
}

LoadedModule load_apk_library(const std::filesystem::path& apk, const std::string& member) {
  const auto apk_member = read_stored_apk_member(apk, member);
  validate_elf(apk_member.bytes);
  const int fd = static_cast<int>(::syscall(SYS_memfd_create, "nuah-module", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_EXEC));
  if (fd < 0) throw std::runtime_error("memfd_create failed");
  try {
    if (::ftruncate(fd, static_cast<off_t>(apk_member.bytes.size())) != 0) throw std::runtime_error("memfd truncate failed");
    write_all(fd, apk_member.bytes);
    if (::fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) throw std::runtime_error("memfd seal failed");
    const std::string path = "/proc/self/fd/" + std::to_string(fd);
    void* handle = ::dlmopen(LM_ID_NEWLM, path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error(std::string("dlmopen failed: ") + ::dlerror());
    LoadedModule result; result.fd_ = fd; result.handle_ = handle; result.size_ = apk_member.bytes.size();
    return result;
  } catch (...) { ::close(fd); throw; }
}
}  // namespace nuah
