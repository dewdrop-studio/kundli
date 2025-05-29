#include "kundli.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

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
    archive->lazy_loaded = false; // Created archives are not lazy loaded
    return archive;
}

unique_ptr<Archive> Archive::load(const string &path) {
    ifstream file(path, ios::binary);
    if (!file) {
        cerr << "Failed to open archive: " << path << '\n';
        return nullptr;
    }

    auto archive = create();
    archive->archive_file_path = path;
    archive->lazy_loaded = true;

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

    // Record the position where data section starts for lazy loading
    u64 data_size = 0;
    file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
    archive->data_section_offset = file.tellg();

    // Don't load the actual data yet for lazy loading
    // We'll validate CRC32 only when data is accessed

    return archive;
}

unique_ptr<Archive> Archive::load_full(const string &path) {
    ifstream file(path, ios::binary);
    if (!file) {
        cerr << "Failed to open archive: " << path << '\n';
        return nullptr;
    }

    auto archive = create();
    archive->lazy_loaded = false; // Traditional full loading

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
        file.read(file_entry.path.data(),
                  static_cast<streamsize>(file_entry.path_length));

        archive->files.push_back(std::move(file_entry));
    }

    // Load all data immediately for traditional full loading
    u64 data_size = 0;
    file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
    archive->data.resize(static_cast<size_t>(data_size));
    file.read(reinterpret_cast<char *>(archive->data.data()),
              static_cast<streamsize>(data_size));

    // Validate CRC32 for full loading
    u32 actual_crc =
        crc32(archive->data.data(), static_cast<size_t>(archive->data.size()));
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

void Archive::compress_parallel(const string &output_path,
                                size_t num_threads) const {
    // Determine optimal thread count
    if (num_threads == 0) {
        num_threads = thread_count > 0 ? thread_count
                                       : std::thread::hardware_concurrency();
    }

    num_threads = std::min(num_threads, files.size());

    if (num_threads <= 1) {
        // Fallback to single-thread
        compress(output_path);
        return;
    }

    ArchiveHeader header_copy = header;
    header_copy.crc32 = crc32(data.data(), data.size());

    // First, write header and file table sequentially
    ofstream out(output_path, ios::binary);
    if (!out) {
        cerr << "Failed to open output: " << output_path << '\n';
        return;
    }

    // Header Section
    out.write(reinterpret_cast<const char *>(&header_copy),
              sizeof(header_copy));
    u64 file_count = files.size();
    out.write(reinterpret_cast<const char *>(&file_count), sizeof(file_count));

    // File Table Section
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
        out.write(file_entry.path.c_str(),
                  static_cast<streamsize>(file_entry.path_length));
    }

    // Write data size placeholder and calculate data section offset
    u64 data_size = data.size();
    out.write(reinterpret_cast<const char *>(&data_size), sizeof(data_size));
    u64 data_start_offset = out.tellp();

    // Pre-allocate space for data section
    out.seekp(data_start_offset + static_cast<std::streamoff>(data_size) - 1);
    out.write("", 1);
    out.close();

    // Now write data section in parallel using random access
    if (data_size > 0) {
        vector<thread> workers;
        workers.reserve(num_threads);
        atomic<size_t> current_chunk{0};
        mutex error_mutex;
        bool has_error = false;
        string error_message;

        // Calculate chunk size for parallel processing
        const size_t min_chunk_size = 64 * 1024; // 64KB minimum chunk
        const size_t chunk_size = std::max(
            min_chunk_size, data_size / (static_cast<size_t>(num_threads) * 4));
        const size_t total_chunks = (data_size + chunk_size - 1) / chunk_size;

        auto worker = [&]() {
            while (true) {
                size_t chunk_idx = current_chunk.fetch_add(1);
                if (chunk_idx >= total_chunks)
                    break;

                size_t chunk_start = chunk_idx * chunk_size;
                size_t chunk_end =
                    std::min(chunk_start + chunk_size, data_size);
                size_t actual_chunk_size = chunk_end - chunk_start;

                try {
                    // Open file for random access writing
                    ofstream chunk_out(output_path,
                                       ios::binary | ios::in | ios::out);
                    if (!chunk_out) {
                        lock_guard<mutex> lock(error_mutex);
                        has_error = true;
                        error_message =
                            "Failed to open file for parallel writing: " +
                            output_path;
                        return;
                    }

                    // Seek to the correct position in the data section
                    chunk_out.seekp(data_start_offset +
                                    static_cast<std::streamoff>(chunk_start));

                    // Write this chunk of data
                    chunk_out.write(reinterpret_cast<const char *>(data.data() +
                                                                   chunk_start),
                                    static_cast<streamsize>(actual_chunk_size));

                    if (!chunk_out.good()) {
                        lock_guard<mutex> lock(error_mutex);
                        has_error = true;
                        error_message = "Failed to write data chunk";
                        return;
                    }

                    chunk_out.close();

                    if (verbose) {
                        lock_guard<mutex> lock(error_mutex);
                        cout << "Wrote chunk " << chunk_idx + 1 << "/"
                             << total_chunks << " (" << actual_chunk_size
                             << " bytes)" << '\n';
                    }
                } catch (const exception &e) {
                    lock_guard<mutex> lock(error_mutex);
                    has_error = true;
                    error_message = string("Error writing chunk: ") + e.what();
                    return;
                }
            }
        };

        // Start worker threads
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back(worker);
        }

        // Wait for all workers to complete
        for (auto &w : workers) {
            w.join();
        }

        // Check for errors
        if (has_error) {
            cerr << "Parallel compression failed: " << error_message << '\n';
            return;
        }

        if (verbose) {
            cout << "Parallel compression completed successfully using "
                 << num_threads << " threads" << '\n';
        }
    }
}

void Archive::decompress() {
    for (const auto &file_entry : files) {
        if (verbose) {
            cout << "Extracting: " << file_entry.path << '\n';
        }

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
                auto file_data = get_file_data(file_entry);
                if (file_data.empty()) {
                    cerr << "Failed to read file data for: " << file_entry.path
                         << '\n';
                    continue;
                }

                ofstream output_file(file_entry.path, ios::binary);
                if (!output_file) {
                    cerr << "Failed to create file: " << file_entry.path
                         << '\n';
                    continue;
                }

                output_file.write(
                    reinterpret_cast<const char *>(file_data.data()),
                    (long)file_data.size());
            } else {
                // Create empty file
                ofstream output_file(file_entry.path);
            }
            break;
        }

        case ArchiveFile::FileType::Symlink: {
            // For symlinks, the target path is stored in the data
            if (file_entry.data_length > 0) {
                auto target_data = get_file_data(file_entry);
                if (target_data.empty()) {
                    cerr << "Failed to read symlink target for: "
                         << file_entry.path << '\n';
                    continue;
                }

                string target(
                    reinterpret_cast<const char *>(target_data.data()),
                    target_data.size());

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
}

void Archive::decompress_parallel(size_t num_threads) {
    // Determine optimal thread count
    if (num_threads == 0) {
        num_threads = thread_count > 0 ? thread_count
                                       : std::thread::hardware_concurrency();
        if (num_threads == 0)
            num_threads = 4; // fallback
    }

    // Limit threads for small workloads
    size_t regular_files = 0;
    for (const auto &file_entry : files) {
        if (file_entry.type == ArchiveFile::FileType::Regular &&
            file_entry.data_length > 0) {
            regular_files++;
        }
    }

    num_threads = std::min(num_threads, std::max(regular_files, size_t(1)));
    if (num_threads <= 1) {
        // Fall back to single-threaded for small workloads
        decompress();
        return;
    }

    if (verbose) {
        cout << "Using " << num_threads << " threads for extraction\n";
    }

    std::atomic<size_t> completed_files{0};
    std::mutex cout_mutex;
    std::mutex fs_mutex; // For directory creation and permission setting

    // Create all directories first (single-threaded to avoid race conditions)
    for (const auto &file_entry : files) {
        if (file_entry.type == ArchiveFile::FileType::Directory) {
            if (verbose) {
                std::lock_guard<std::mutex> lock(cout_mutex);
                cout << "Creating directory: " << file_entry.path << '\n';
            }
            fs::create_directories(file_entry.path);

            // Set directory permissions
            try {
                auto perms = static_cast<fs::perms>(
                    (file_entry.permissions[0] << 6) | // owner
                    (file_entry.permissions[1] << 3) | // group
                    (file_entry.permissions[2])        // others
                );
                fs::permissions(file_entry.path, perms);
            } catch (const fs::filesystem_error &e) {
                std::lock_guard<std::mutex> lock(cout_mutex);
                cerr << "Failed to set permissions for directory: "
                     << file_entry.path << ": " << e.what() << '\n';
            }
        }
    }

    // Create parent directories for all files first
    for (const auto &file_entry : files) {
        if (file_entry.type != ArchiveFile::FileType::Directory) {
            fs::path file_path(file_entry.path);
            if (file_path.has_parent_path()) {
                fs::create_directories(file_path.parent_path());
            }
        }
    }

    // Worker function for extracting files
    auto extract_worker = [&](size_t start_idx, size_t end_idx) {
        for (size_t i = start_idx; i < end_idx; ++i) {
            const auto &file_entry = files[i];

            // Skip directories (already processed)
            if (file_entry.type == ArchiveFile::FileType::Directory) {
                continue;
            }

            if (verbose) {
                std::lock_guard<std::mutex> lock(cout_mutex);
                cout << "Extracting: " << file_entry.path << '\n';
            }

            switch (file_entry.type) {
            case ArchiveFile::FileType::Regular: {
                if (file_entry.data_length > 0) {
                    auto file_data = get_file_data(file_entry);
                    if (file_data.empty()) {
                        std::lock_guard<std::mutex> lock(cout_mutex);
                        cerr << "Failed to read file data for: "
                             << file_entry.path << '\n';
                        continue;
                    }

                    ofstream output_file(file_entry.path, ios::binary);
                    if (!output_file) {
                        std::lock_guard<std::mutex> lock(cout_mutex);
                        cerr << "Failed to create file: " << file_entry.path
                             << '\n';
                        continue;
                    }

                    output_file.write(
                        reinterpret_cast<const char *>(file_data.data()),
                        static_cast<streamsize>(file_data.size()));
                } else {
                    // Create empty file
                    ofstream output_file(file_entry.path);
                }
                break;
            }

            case ArchiveFile::FileType::Symlink: {
                if (file_entry.data_length > 0) {
                    auto target_data = get_file_data(file_entry);
                    if (target_data.empty()) {
                        std::lock_guard<std::mutex> lock(cout_mutex);
                        cerr << "Failed to read symlink target for: "
                             << file_entry.path << '\n';
                        continue;
                    }

                    string target(
                        reinterpret_cast<const char *>(target_data.data()),
                        target_data.size());

                    try {
                        fs::create_symlink(target, file_entry.path);
                    } catch (const fs::filesystem_error &e) {
                        std::lock_guard<std::mutex> lock(cout_mutex);
                        cerr << "Failed to create symlink: " << file_entry.path
                             << " -> " << target << ": " << e.what() << '\n';
                    }
                }
                break;
            }

            case ArchiveFile::FileType::Directory:
                // Already handled above
                break;
            }

            // Set file permissions (protect with mutex for thread safety)
            {
                std::lock_guard<std::mutex> lock(fs_mutex);
                try {
                    auto perms = static_cast<fs::perms>(
                        (file_entry.permissions[0] << 6) | // owner
                        (file_entry.permissions[1] << 3) | // group
                        (file_entry.permissions[2])        // others
                    );
                    fs::permissions(file_entry.path, perms);
                } catch (const fs::filesystem_error &e) {
                    std::lock_guard<std::mutex> cout_lock(cout_mutex);
                    cerr << "Failed to set permissions for: " << file_entry.path
                         << ": " << e.what() << '\n';
                }
            }

            completed_files.fetch_add(1);
        }
    };

    // Divide work among threads
    std::vector<std::thread> workers;
    size_t files_per_thread = files.size() / num_threads;
    size_t remaining_files = files.size() % num_threads;

    for (size_t t = 0; t < num_threads; ++t) {
        size_t start_idx = t * files_per_thread;
        size_t end_idx = start_idx + files_per_thread;

        // Distribute remaining files to the first few threads
        if (t < remaining_files) {
            end_idx++;
            start_idx += t;
        } else {
            start_idx += remaining_files;
            end_idx += remaining_files;
        }

        if (start_idx < files.size()) {
            workers.emplace_back(extract_worker, start_idx,
                                 std::min(end_idx, files.size()));
        }
    }

    for (auto &worker : workers) {
        worker.join();
    }

    if (verbose) {
        cout << "Extracted" << "\n";
    }
}

void Archive::list_files() const {
    if (files.empty()) {
        cout << "Archive is empty\n";
        return;
    }

    cout << "total " << files.size() << "\n";

    for (const ArchiveFile &f : files) {
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

            std::vector<u8> target_data = get_file_data(f);
            if (!target_data.empty()) {
                string target(
                    reinterpret_cast<const char *>(target_data.data()),
                    target_data.size());
                cout << " -> " << target;
            } else {
                cout << " -> <unavailable>";
            }
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
    if (lazy_loaded && data.empty()) {
        cout << "Data: Not loaded (lazy loading enabled)\n";
    } else {
        cout << "Data Size: " << data.size() << " bytes\n";
    }
}

void Archive::load_file_data_if_needed() {
    if (!lazy_loaded || !data.empty()) {
        return; // Already loaded or not using lazy loading
    }

    ifstream file(archive_file_path, ios::binary);
    if (!file) {
        cerr << "Failed to reopen archive for lazy loading: "
             << archive_file_path << '\n';
        return;
    }

    // Seek to data section
    file.seekg(static_cast<streamoff>(data_section_offset));

    // Read data size (it was already read during initial load but we need to
    // skip it)
    u64 data_size = 0;
    file.seekg(static_cast<streamoff>(data_section_offset - sizeof(u64)));
    file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));

    // Now read the actual data
    data.resize(static_cast<size_t>(data_size));
    file.read(reinterpret_cast<char *>(data.data()),
              static_cast<streamsize>(data_size));

    // Validate CRC32 now that we have the data
    u32 actual_crc = crc32(data.data(), static_cast<size_t>(data.size()));
    if (header.crc32 != actual_crc) {
        cerr << "Archive CRC32 mismatch! The archive may be corrupted.\n";
        data.clear(); // Clear potentially corrupted data
        return;
    }

    if (verbose) {
        cout << "Loaded " << data_size << " bytes of archive data\n";
    }
}

const std::vector<u8> Archive::get_file_data(const ArchiveFile &file) const {
    if (file.type == ArchiveFile::FileType::Directory) {
        return {}; // Directories have no data
    }

    if (lazy_loaded) {
        // For lazy loading, read the specific file data directly from disk
        ifstream archive_file(archive_file_path, ios::binary);
        if (!archive_file) {
            cerr << "Failed to open archive for reading file data: "
                 << archive_file_path << '\n';
            return {};
        }

        // Seek to the file's data position
        u64 absolute_offset = data_section_offset + file.offset;
        archive_file.seekg(static_cast<streamoff>(absolute_offset));

        std::vector<u8> file_data(static_cast<size_t>(file.data_length));
        archive_file.read(reinterpret_cast<char *>(file_data.data()),
                          static_cast<streamsize>(file.data_length));

        if (verbose) {
            cout << "Lazy loaded " << file.data_length
                 << " bytes for file: " << file.path << '\n';
        }

        return file_data;
    } else {
        // Traditional loading: data is already in memory
        if (file.offset + file.data_length > data.size()) {
            cerr << "File data extends beyond archive data: " << file.path
                 << '\n';
            return {};
        }

        std::vector<u8> file_data(static_cast<size_t>(file.data_length));
        auto start_it =
            data.begin() +
            static_cast<std::vector<u8>::difference_type>(file.offset);
        auto end_it =
            data.begin() + static_cast<std::vector<u8>::difference_type>(
                               file.offset + file.data_length);
        std::copy(start_it, end_it, file_data.begin());
        return file_data;
    }
}

const std::vector<u8>
Archive::get_file_data(const std::string &file_path) const {
    auto it =
        std::find_if(files.begin(), files.end(),
                     [&](const ArchiveFile &f) { return f.path == file_path; });

    if (it == files.end()) {
        cerr << "File not found in archive: " << file_path << '\n';
        return {};
    }

    return get_file_data(*it);
}

void Archive::decompress_file(const std::string &file_path,
                              const std::string &output_path) {
    auto it =
        std::find_if(files.begin(), files.end(),
                     [&](const ArchiveFile &f) { return f.path == file_path; });

    if (it == files.end()) {
        cerr << "File not found in archive: " << file_path << '\n';
        return;
    }

    const ArchiveFile &file_entry = *it;

    if (verbose) {
        cout << "Extracting: " << file_entry.path << " to " << output_path
             << '\n';
    }

    // Create parent directories if they don't exist
    fs::path out_path(output_path);
    if (out_path.has_parent_path()) {
        fs::create_directories(out_path.parent_path());
    }

    switch (file_entry.type) {
    case ArchiveFile::FileType::Directory: {
        fs::create_directories(output_path);
        break;
    }

    case ArchiveFile::FileType::Regular: {
        if (file_entry.data_length > 0) {
            auto file_data = get_file_data(file_entry);
            if (file_data.empty()) {
                cerr << "Failed to read file data for: " << file_entry.path
                     << '\n';
                return;
            }

            ofstream output_file(output_path, ios::binary);
            if (!output_file) {
                cerr << "Failed to create file: " << output_path << '\n';
                return;
            }

            output_file.write(reinterpret_cast<const char *>(file_data.data()),
                              (long)file_data.size());
        } else {
            // Create empty file
            ofstream output_file(output_path);
        }
        break;
    }

    case ArchiveFile::FileType::Symlink: {
        if (file_entry.data_length > 0) {
            auto target_data = get_file_data(file_entry);
            if (target_data.empty()) {
                cerr << "Failed to read symlink target for: " << file_entry.path
                     << '\n';
                return;
            }

            string target(reinterpret_cast<const char *>(target_data.data()),
                          target_data.size());

            try {
                fs::create_symlink(target, output_path);
            } catch (const fs::filesystem_error &e) {
                cerr << "Failed to create symlink: " << output_path << " -> "
                     << target << ": " << e.what() << '\n';
            }
        }
        break;
    }
    }

    // Restore permissions
    try {
        auto perms =
            static_cast<fs::perms>((file_entry.permissions[0] << 6) | // owner
                                   (file_entry.permissions[1] << 3) | // group
                                   (file_entry.permissions[2])        // others
            );
        fs::permissions(output_path, perms);
    } catch (const fs::filesystem_error &e) {
        cerr << "Failed to set permissions for: " << output_path << ": "
             << e.what() << '\n';
    }
}
