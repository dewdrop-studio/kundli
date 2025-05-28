#include "kundli.hpp"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

using namespace std;
namespace fs = std::filesystem;

u32 crc32(const u8 *data, size_t length) {
    u32 crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

unique_ptr<Archive> Archive::create() {
    auto archive = unique_ptr<Archive>(new Archive());
    strncpy(reinterpret_cast<char *>(archive->header.magic), ARCHIVE_MAGIC, 5);
    archive->header.version = ARCHIVE_VERSION;
    archive->header.flags = static_cast<u8>(ArchiveFlag::None);
    archive->header.timestamp = static_cast<u64>(time(nullptr));
    return archive;
}

unique_ptr<Archive> Archive::load(const string &path) {
    ifstream file(path, ios::binary);
    if (!file) {
        cerr << "Failed to open archive: " << path << '\n';
        return nullptr;
    }

    auto archive = create();
    file.read(reinterpret_cast<char *>(&archive->header),
              sizeof(ArchiveHeader));

    if (strncmp(reinterpret_cast<char *>(archive->header.magic), ARCHIVE_MAGIC,
                4) != 0 ||
        archive->header.version != ARCHIVE_VERSION) {
        cerr << "Invalid archive format or version mismatch.\n";
        return nullptr;
    }

    u64 file_count = 0;
    file.read(reinterpret_cast<char *>(&file_count), sizeof(file_count));

    archive->files.reserve(file_count);
    for (u64 i = 0; i < file_count; ++i) {
        ArchiveFile file_entry;
        file.read(reinterpret_cast<char *>(&file_entry.offset),
                  sizeof(file_entry.offset));
        file.read(reinterpret_cast<char *>(&file_entry.size),
                  sizeof(file_entry.size));
        file.read(reinterpret_cast<char *>(file_entry.permissions), 3);
        file.read(reinterpret_cast<char *>(&file_entry.type),
                  sizeof(file_entry.type));
        file.read(reinterpret_cast<char *>(&file_entry.path_length),
                  sizeof(file_entry.path_length));
        file.read(reinterpret_cast<char *>(&file_entry.data_length),
                  sizeof(file_entry.data_length));

        // Read the path string
        file_entry.path.resize(file_entry.path_length);
        file.read(file_entry.path.data(), (long)file_entry.path_length);

        archive->files.push_back(std::move(file_entry));
    }

    u64 data_size = 0;
    file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
    archive->data.resize(data_size);
    file.read(reinterpret_cast<char *>(archive->data.data()), (long)data_size);

    u32 actual_crc = crc32(archive->data.data(), archive->data.size());
    if (archive->header.crc32 != actual_crc) {
        cerr << "Archive CRC32 mismatch! The archive may be corrupted.\n";
        return nullptr;
    }

    return archive;
}

Archive::~Archive() = default;

ArchiveFile *Archive::add_file(const string &path) {
    if (!fs::exists(path)) {
        cerr << "File does not exist: " << path << '\n';
        return nullptr;
    }

    // Normalize the path
    string normalized_path = normalize_path(path);

    // If it's a directory, branch to add_directory
    if (fs::is_directory(normalized_path)) {
        return add_directory(normalized_path);
    }

    // Check if this file is already in the archive
    auto existing =
        std::find_if(files.begin(), files.end(), [&](const ArchiveFile &f) {
            return f.path == normalized_path &&
                   f.type != ArchiveFile::FileType::Directory;
        });

    if (existing != files.end()) {
        if (verbose) {
            cerr << "File already exists in archive, skipping: "
                 << normalized_path << '\n';
        }
        return &(*existing);
    }

    // Ensure all parent directories are added to the archive
    add_parent_directories(normalized_path);

    ArchiveFile file_entry;
    file_entry.path = normalized_path;
    file_entry.path_length = normalized_path.size();
    file_entry.offset = data.size();

    auto perms = fs::status(normalized_path).permissions();
    file_entry.permissions[0] =
        static_cast<u8>((static_cast<u32>(perms) >> 6) & 0b111); // owner
    file_entry.permissions[1] =
        static_cast<u8>((static_cast<u32>(perms) >> 3) & 0b111); // group
    file_entry.permissions[2] =
        static_cast<u8>((static_cast<u32>(perms)) & 0b111); // others

    if (fs::is_symlink(normalized_path)) {
        file_entry.type = ArchiveFile::FileType::Symlink;
        // For symlinks, store the target path as data
        string target = fs::read_symlink(normalized_path).string();
        file_entry.data_length = target.size();
        file_entry.size = file_entry.data_length + file_entry.path_length;

        data.insert(data.end(), target.begin(), target.end());
    } else {
        file_entry.type = ArchiveFile::FileType::Regular;
        file_entry.data_length = fs::file_size(normalized_path);
        file_entry.size = file_entry.data_length + file_entry.path_length;

        ifstream file(normalized_path, ios::binary);
        if (!file) {
            cerr << "Failed to open: " << normalized_path << '\n';
            return nullptr;
        }

        vector<u8> buffer(file_entry.data_length);
        file.read(reinterpret_cast<char *>(buffer.data()),
                  (long)file_entry.data_length);
        data.insert(data.end(), buffer.begin(), buffer.end());
    }

    files.push_back(std::move(file_entry));
    return &files.back();
}

ArchiveFile *Archive::add_directory(const string &path) {
    if (!fs::exists(path)) {
        cerr << "Directory does not exist: " << path << '\n';
        return nullptr;
    }

    if (!fs::is_directory(path)) {
        cerr << "Path is not a directory: " << path << '\n';
        return nullptr;
    }

    // Normalize the path
    string normalized_path = normalize_path(path);

    // Ensure all parent directories are added to the archive
    add_parent_directories(normalized_path);

    // Check if this directory is already in the archive
    auto existing =
        std::find_if(files.begin(), files.end(), [&](const ArchiveFile &f) {
            return f.path == normalized_path &&
                   f.type == ArchiveFile::FileType::Directory;
        });

    if (existing != files.end()) {
        // Directory already exists, return pointer to it
        if (verbose) {
            cerr << "Directory already exists in archive, skipping: "
                 << normalized_path << '\n';
        }
        return &(*existing);
    }

    // First, add the directory entry itself
    ArchiveFile dir_entry;
    dir_entry.path = normalized_path;
    dir_entry.path_length = normalized_path.size();
    dir_entry.type = ArchiveFile::FileType::Directory;
    dir_entry.data_length = 0;              // Directories have no data
    dir_entry.offset = data.size();         // Current position in data
    dir_entry.size = dir_entry.path_length; // Only path length for directories

    auto perms = fs::status(normalized_path).permissions();
    dir_entry.permissions[0] =
        static_cast<u8>((static_cast<u32>(perms) >> 6) & 0b111); // owner
    dir_entry.permissions[1] =
        static_cast<u8>((static_cast<u32>(perms) >> 3) & 0b111); // group
    dir_entry.permissions[2] =
        static_cast<u8>((static_cast<u32>(perms)) & 0b111); // others

    files.push_back(std::move(dir_entry));
    ArchiveFile *result = &files.back();

    // Recursively add all contents of the directory
    try {
        for (const auto &entry : fs::directory_iterator(normalized_path)) {
            const string entry_path = entry.path().string();

            if (entry.is_directory()) {
                add_directory(entry_path); // Recursive call for subdirectories
            } else {
                add_file(entry_path); // Add files and symlinks
            }
        }
    } catch (const fs::filesystem_error &e) {
        cerr << "Error reading directory " << normalized_path << ": "
             << e.what() << '\n';
    }

    return result;
}

void Archive::add_parent_directories(const string &path) {
    fs::path file_path(path);

    // Collect all parent directories
    vector<string> parent_dirs;
    fs::path current_parent = file_path.parent_path();

    while (!current_parent.empty() &&
           current_parent != current_parent.root_path()) {
        parent_dirs.push_back(current_parent.string());
        current_parent = current_parent.parent_path();
    }

    // Add parent directories in reverse order (from root to immediate parent)
    for (auto it = parent_dirs.rbegin(); it != parent_dirs.rend(); ++it) {
        const string &parent_dir = *it;

        // Check if this directory is already in the archive
        auto existing =
            std::find_if(files.begin(), files.end(), [&](const ArchiveFile &f) {
                return f.path == parent_dir &&
                       f.type == ArchiveFile::FileType::Directory;
            });

        // If not found
        if (existing == files.end()) {
            if (fs::exists(parent_dir) && fs::is_directory(parent_dir)) {
                // Add directory entry without recursively adding its contents
                ArchiveFile dir_entry;
                dir_entry.path = parent_dir;
                dir_entry.path_length = parent_dir.size();
                dir_entry.type = ArchiveFile::FileType::Directory;
                dir_entry.data_length = 0;
                dir_entry.offset = data.size();
                dir_entry.size = dir_entry.path_length;

                auto perms = fs::status(parent_dir).permissions();
                dir_entry.permissions[0] = static_cast<u8>(
                    (static_cast<u32>(perms) >> 6) & 0b111); // owner
                dir_entry.permissions[1] = static_cast<u8>(
                    (static_cast<u32>(perms) >> 3) & 0b111); // group
                dir_entry.permissions[2] = static_cast<u8>(
                    (static_cast<u32>(perms)) & 0b111); // others

                files.push_back(std::move(dir_entry));
            }
        }
    }
}

string Archive::normalize_path(const string &path) {
    fs::path normalized = fs::path(path).lexically_normal();
    string result = normalized.string();

    // Remove trailing slash for directories except root
    if (result.length() > 1 && result.back() == '/') {
        result.pop_back();
    }

    return result;
}

void Archive::remove_file(const string &path) {
    auto it =
        std::find_if(files.begin(), files.end(),
                     [&](const ArchiveFile &f) { return f.path == path; });
    if (it != files.end()) {
        files.erase(it);
    } else {
        cerr << "File not found: " << path << '\n';
    }
}

void Archive::compress(const string &output_path) const {
    ArchiveHeader header_copy = header;
    header_copy.crc32 = crc32(data.data(), data.size());

    ofstream out(output_path, ios::binary);
    if (!out) {
        cerr << "Failed to open output: " << output_path << '\n';
        return;
    }

    out.write(reinterpret_cast<const char *>(&header_copy),
              sizeof(header_copy));
    u64 file_count = files.size();
    out.write(reinterpret_cast<const char *>(&file_count), sizeof(file_count));

    for (const auto &file_entry : files) {
        out.write(reinterpret_cast<const char *>(&file_entry.offset),
                  sizeof(file_entry.offset));
        out.write(reinterpret_cast<const char *>(&file_entry.size),
                  sizeof(file_entry.size));
        out.write(reinterpret_cast<const char *>(file_entry.permissions), 3);
        out.write(reinterpret_cast<const char *>(&file_entry.type),
                  sizeof(file_entry.type));
        out.write(reinterpret_cast<const char *>(&file_entry.path_length),
                  sizeof(file_entry.path_length));
        out.write(reinterpret_cast<const char *>(&file_entry.data_length),
                  sizeof(file_entry.data_length));
        out.write(file_entry.path.c_str(), (long)file_entry.path_length);
    }

    u64 data_size = data.size();
    out.write(reinterpret_cast<const char *>(&data_size), sizeof(data_size));
    out.write(reinterpret_cast<const char *>(data.data()), (long)data_size);
}

void Archive::decompress() {
    for (const auto &file_entry : files) {
        cout << "Extracting: " << file_entry.path << '\n';

        // Create parent directories if they don't exist
        fs::path file_path(file_entry.path);
        if (file_path.has_parent_path()) {
            fs::create_directories(file_path.parent_path());
        }

        switch (file_entry.type) {
        case ArchiveFile::FileType::Directory: {
            fs::create_directories(file_entry.path);
            break;
        }

        case ArchiveFile::FileType::Regular: {
            if (file_entry.data_length > 0) {
                ofstream output_file(file_entry.path, ios::binary);
                if (!output_file) {
                    cerr << "Failed to create file: " << file_entry.path
                         << '\n';
                    continue;
                }

                // Extract data from the archive's data buffer
                const u8 *file_data = data.data() + file_entry.offset;
                output_file.write(reinterpret_cast<const char *>(file_data),
                                  (long)file_entry.data_length);
            } else {
                // Create empty file
                ofstream output_file(file_entry.path);
            }
            break;
        }

        case ArchiveFile::FileType::Symlink: {
            // For symlinks, the target path is stored in the data
            if (file_entry.data_length > 0) {
                const u8 *target_data = data.data() + file_entry.offset;
                string target(reinterpret_cast<const char *>(target_data),
                              file_entry.data_length);

                try {
                    fs::create_symlink(target, file_entry.path);
                } catch (const fs::filesystem_error &e) {
                    cerr << "Failed to create symlink: " << file_entry.path
                         << " -> " << target << ": " << e.what() << '\n';
                }
            }
            break;
        }
        }

        // Restore permissions
        try {
            auto perms = static_cast<fs::perms>(
                (file_entry.permissions[0] << 6) | // owner
                (file_entry.permissions[1] << 3) | // group
                (file_entry.permissions[2])        // others
            );
            fs::permissions(file_entry.path, perms);
        } catch (const fs::filesystem_error &e) {
            cerr << "Failed to set permissions for: " << file_entry.path << ": "
                 << e.what() << '\n';
        }
    }

    cout << "Extracted " << files.size() << " files\n";
}

void Archive::list_files() const {
    if (files.empty()) {
        cout << "Archive is empty\n";
        return;
    }

    cout << "total " << files.size() << "\n";

    for (const auto &f : files) {
        // File type and permissions (like ls -l)
        char type_char = '-';
        switch (f.type) {
        case ArchiveFile::FileType::Directory:
            type_char = 'd';
            break;
        case ArchiveFile::FileType::Symlink:
            type_char = 'l';
            break;
        case ArchiveFile::FileType::Regular:
            type_char = '.';
            break;
        }

        // Format permissions as rwxrwxrwx
        auto format_perms = [](u8 perm) -> string {
            string result;
            result += (perm & 0b100) ? 'r' : '-';
            result += (perm & 0b010) ? 'w' : '-';
            result += (perm & 0b001) ? 'x' : '-';
            return result;
        };

        string owner_perms = format_perms(f.permissions[0]);
        string group_perms = format_perms(f.permissions[1]);
        string other_perms = format_perms(f.permissions[2]);

        // Format size with right alignment (similar to ls)
        string size_str = to_string(f.data_length);

        // Print in ls -l format: type+permissions size path
        cout << type_char << owner_perms << group_perms << other_perms;
        cout << " " << setw(8) << right << size_str;
        cout << " " << f.path;

        // For symlinks, show target if available
        if (f.type == ArchiveFile::FileType::Symlink && f.data_length > 0) {
            const u8 *target_data = data.data() + f.offset;
            string target(reinterpret_cast<const char *>(target_data),
                          f.data_length);
            cout << " -> " << target;
        }

        cout << "\n";
    }
}

void Archive::print_info() const {
    cout << "Version: " << static_cast<int>(header.version) << '\n';
    cout << "Flags: " << static_cast<int>(header.flags) << '\n';
    cout << "CRC32: " << hex << setfill('0') << setw(8) << header.crc32 << dec
         << '\n';
    cout << "Files: " << files.size() << '\n';
    cout << "Data Size: " << data.size() << " bytes\n";
}
