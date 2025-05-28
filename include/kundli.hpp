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

constexpr const char *ARCHIVE_MAGIC = "KNDL";
constexpr u8 ARCHIVE_VERSION = 1;

enum class ArchiveFlag : u8 {
    None = 1 << 0,
    Compressed = 1 << 1,
    Encrypted = 1 << 2,
};

struct ArchiveHeader {
    u8 magic[5]{}; // Magic number to identify the archive format (KNDL + '\0')
    u8 version{};  // Version of the archive format
    u8 flags{};    // Bitmask of ArchiveFlag
    u64 timestamp{}; // Timestamp of the archive creation
    u32 crc32{};     // CRC32 checksum
} __attribute__((packed));

struct ArchiveFile {
    u64 offset{};        // Offset in the archive data where the file starts
    u64 size{};          // Size of the file data in bytes
    u8 permissions[3]{}; // Permissions for owner, group, and others (3 bytes)
    enum class FileType : u8 {
        Regular = 0,
        Directory,
        Symlink
    } type{};          // Type of the file (Regular, Directory, Symlink)
    u64 path_length{}; // Length of the file path
    u64 data_length{}; // Length of the file data (for Regular files) or symlink
                       // target
    std::string path{}; // File path relative to the archive root

    ArchiveFile() = default;
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

    void set_verbose(bool verbose) { this->verbose = verbose; }

  private:
    Archive() = default;

    void add_parent_directories(const std::string &path);
    std::string normalize_path(const std::string &path);

    ArchiveHeader header{};
    std::vector<ArchiveFile> files;
    std::vector<u8> data;
    bool verbose{false};
};
