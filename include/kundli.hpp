#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr const char *ARCHIVE_MAGIC = "KUNDLI";
constexpr u8 ARCHIVE_VERSION = 1;

enum class ArchiveFlag : u8 {
  None = 1 << 0,
  Compressed = 1 << 1,
  Encrypted = 1 << 2,
};

struct ArchiveHeader {
  u8 magic[7]{};   // Magic number to identify the archive format
  u8 version{};    // Version of the archive format
  u8 flags{};      // Bitmask of ArchiveFlag
  u64 timestamp{}; // Timestamp of the archive creation
} __attribute__((packed));

enum class FileType : u8 { Regular = 0, Directory, Symlink };

struct ArchiveFile {
  u64 offset{};
  u64 size{};
  u8 permissions[3]{};
  FileType type{};
  u64 path_length{};
  u64 data_length{};
  std::string path{};
};

class Archive {
public:
  static std::unique_ptr<Archive> create();
  static std::unique_ptr<Archive> load(const std::string &path);
  ~Archive();

  ArchiveFile *add_file(const std::string &path);
  ArchiveFile *add_directory(const std::string &path);

  void remove_file(const std::string &path);

  void compress(const std::string &output_path) const;
  void decompress();

  void list_files() const;
  void print_info() const;

private:
  Archive() = default;

  void add_parent_directories(const std::string &path);

  ArchiveHeader header{};
  std::vector<ArchiveFile> files;
  std::vector<u8> data;
};
