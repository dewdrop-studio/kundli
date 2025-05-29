#include "kundli.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// SIMD support
#ifdef __x86_64__
#include <cpuid.h>
#include <immintrin.h>
#endif

// Memory mapping support
#ifdef __unix__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace std;
namespace fs = std::filesystem;

// Optimized CRC32 implementation with lookup table

constexpr std::array<u32, 256> create_crc32_table() {
    std::array<u32, 256> table{};

    for (u32 i = 0; i < 256; ++i) {
        u32 crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
        table[i] = crc;
    }
    return table;
}
constexpr std::array<u32, 256> crc32_table = create_crc32_table();

u32 crc32(const u8 *data, size_t length) {
    u32 crc = 0xFFFFFFFF;

    for (size_t i = 0; i < length; ++i) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }

    return ~crc;
}

// SIMD-optimized CRC32 implementation
#ifdef __SSE4_2__
bool has_sse4_2() {
    static bool checked = false;
    static bool supported = false;

    if (!checked) {
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            supported = (ecx & bit_SSE4_2) != 0;
        }
        checked = true;
    }
    return supported;
}

// Hardware-accelerated CRC32 using SSE4.2 instructions
u32 crc32_simd(const u8 *data, size_t length) {
    if (!has_sse4_2()) {
        return crc32(data, length); // Fallback to table-based version
    }

    u32 crc = 0xFFFFFFFF;
    const u8 *end = data + length;

    // Process 8-byte chunks using hardware CRC32
    while (data + 8 <= end) {
        crc = _mm_crc32_u64(crc, *reinterpret_cast<const u64 *>(data));
        data += 8;
    }

    // Process 4-byte chunk if available
    if (data + 4 <= end) {
        crc = _mm_crc32_u32(crc, *reinterpret_cast<const u32 *>(data));
        data += 4;
    }

    // Process remaining bytes
    while (data < end) {
        crc = _mm_crc32_u8(crc, *data);
        data++;
    }

    return ~crc;
}
#endif

// Vectorized memory operations
void *fast_memcpy(void *dest, const void *src, size_t n) {
#ifdef __x86_64__
    if (n >= 32) {
        // Use AVX for large copies if available
        const char *s = static_cast<const char *>(src);
        char *d = static_cast<char *>(dest);

        // Align to 32-byte boundary
        while (((uintptr_t)d & 31) && n > 0) {
            *d++ = *s++;
            n--;
        }

        // Copy 32-byte chunks with AVX
        while (n >= 32) {
            __m256i data =
                _mm256_loadu_si256(reinterpret_cast<const __m256i *>(s));
            _mm256_store_si256(reinterpret_cast<__m256i *>(d), data);
            s += 32;
            d += 32;
            n -= 32;
        }

        // Copy remaining bytes
        while (n > 0) {
            *d++ = *s++;
            n--;
        }

        return dest;
    }
#endif
    return std::memcpy(dest, src, n);
}

// MappedFile implementation for memory mapping large archives
Archive::MappedFile::~MappedFile() { unmap(); }

bool Archive::MappedFile::map_file(const std::string &path) {
#ifdef __unix__
    // Open file for reading
    fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        return false;
    }

    // Get file size
    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        fd = -1;
        return false;
    }

    file_size = static_cast<size_t>(st.st_size);

    // Memory map the file
    mapped_data = static_cast<u8 *>(
        mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mapped_data == MAP_FAILED) {
        mapped_data = nullptr;
        close(fd);
        fd = -1;
        return false;
    }

    // We're gonna do sequential access,
    // so advise the kernel to optimize for that
    madvise(mapped_data, file_size, MADV_SEQUENTIAL);

    return true;
#else
    // Memory mapping not supported on this platform ( Windows :/ )
    return false;
#endif
}

void Archive::MappedFile::unmap() {
#ifdef __unix__
    if (mapped_data != nullptr) {
        munmap(mapped_data, file_size);
        mapped_data = nullptr;
    }
    if (fd != -1) {
        close(fd);
        fd = -1;
    }
#endif
    file_size = 0;
}

Archive::MemoryPool Archive::memory_pool;
Archive::ThreadPool Archive::thread_pool;

// Thread pool
// https://www.geeksforgeeks.org/thread-pool-in-cpp/

// probably gonna be the source of the most headaches
// Wish there was a standard library for this

Archive::ThreadPool::ThreadPool(size_t threads) : stop(false) {
    if (threads == 0)
        threads = std::thread::hardware_concurrency();
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] {
                        return this->stop || !this->tasks.empty();
                    });
                    if (this->stop && this->tasks.empty())
                        return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
        });
    }
}

Archive::ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread &worker : workers) {
        worker.join();
    }
}

void Archive::ThreadPool::resize(size_t new_size) {
    if (new_size == workers.size())
        return;

    // Stop current workers
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread &worker : workers) {
        worker.join();
    }

    // Clear and restart
    workers.clear();
    stop = false;

    for (size_t i = 0; i < new_size; ++i) {
        workers.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this] {
                        return this->stop || !this->tasks.empty();
                    });
                    if (this->stop && this->tasks.empty())
                        return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
        });
    }
}

unique_ptr<Archive> Archive::create() {
    auto archive = unique_ptr<Archive>(new Archive());
    strncpy(reinterpret_cast<char *>(archive->header.magic), ARCHIVE_MAGIC, 5);
    archive->header.version = ARCHIVE_VERSION;
    archive->header.flags = static_cast<u8>(ArchiveFlag::None);
    archive->header.timestamp = static_cast<u64>(time(nullptr));
    archive->lazy_loaded = false;
    // Created archives are not lazy loaded
    // I will find a way to write files directly to the archive sometime later
    // 🤓
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

        file_entry.path.resize(file_entry.path_length);
        file.read(file_entry.path.data(), (long)file_entry.path_length);

        archive->files.push_back(std::move(file_entry));
    }

    u64 data_size = 0;
    file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
    archive->data_section_offset = file.tellg();

    return archive;
}

unique_ptr<Archive> Archive::load_full(const string &path) {
    ifstream file(path, ios::binary);
    if (!file) {
        cerr << "Failed to open archive: " << path << '\n';
        return nullptr;
    }

    auto archive = create();
    archive->lazy_loaded = false;

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

        file_entry.path.resize(file_entry.path_length);
        file.read(file_entry.path.data(),
                  static_cast<streamsize>(file_entry.path_length));

        archive->files.push_back(std::move(file_entry));
    }

    u64 data_size = 0;
    file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
    archive->data.resize(static_cast<size_t>(data_size));
    file.read(reinterpret_cast<char *>(archive->data.data()),
              static_cast<streamsize>(data_size));

    // Validate CRC32 for full loading
#ifdef __x86_64__
    u32 actual_crc = crc32_simd(archive->data.data(),
                                static_cast<size_t>(archive->data.size()));
#else
    u32 actual_crc =
        crc32(archive->data.data(), static_cast<size_t>(archive->data.size()));
#endif
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

    if (fs::is_directory(normalized_path)) {
        return add_directory(normalized_path);
    }

    // Check if this file is already in the archive
    // Todo!: implement a way to update files
    // i can just add the offset difference to every file entry after it 💀
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
        // https://en.wikipedia.org/wiki/Symbolic_link
        // symlink store the link target as data
        // i thought they would be something on the filesystem but ok?
        file_entry.type = ArchiveFile::FileType::Symlink;
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

        // Buffer read files in chunks to avoid larger allocations
        // Note: i should make the size a build option
        constexpr size_t BUFFER_SIZE = 1024UL * 1024UL;
        const size_t file_size = file_entry.data_length;

        // Reserve space for the data section
        // "Might as well use an Arena" - 🤓
        data.reserve(data.size() + file_size);

        if (file_size <= BUFFER_SIZE) {
            // smaller files, just read in one go
            vector<u8> buffer(file_size);
            file.read(reinterpret_cast<char *>(buffer.data()),
                      static_cast<streamsize>(file_size));
            data.insert(data.end(), buffer.begin(), buffer.end());
        } else {

            vector<u8> buffer(BUFFER_SIZE);
            size_t remaining = file_size;

            while (remaining > 0) {
                size_t to_read = std::min(remaining, BUFFER_SIZE);
                file.read(reinterpret_cast<char *>(buffer.data()),
                          static_cast<streamsize>(to_read));

                auto bytes_read = file.gcount();
                if (bytes_read > 0) {
                    data.insert(data.end(), buffer.begin(),
                                buffer.begin() + bytes_read);
                    remaining -= static_cast<size_t>(bytes_read);
                } else {
                    break;
                }
            }
        }
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

    string normalized_path = normalize_path(path);
    add_parent_directories(normalized_path);

    auto existing =
        std::find_if(files.begin(), files.end(), [&](const ArchiveFile &f) {
            return f.path == normalized_path &&
                   f.type == ArchiveFile::FileType::Directory;
        });

    if (existing != files.end()) {
        if (verbose) {
            cerr << "Directory already exists in archive, skipping: "
                 << normalized_path << '\n';
        }
        return &(*existing);
    }

    // create directory first
    // Archive blows up if its not there :/
    ArchiveFile dir_entry;
    dir_entry.path = normalized_path;
    dir_entry.path_length = normalized_path.size();
    dir_entry.type = ArchiveFile::FileType::Directory;
    dir_entry.data_length = 0;
    dir_entry.offset = data.size();
    dir_entry.size = dir_entry.path_length;

    auto perms = fs::status(normalized_path).permissions();
    dir_entry.permissions[0] =
        static_cast<u8>((static_cast<u32>(perms) >> 6) & 0b111); // owner
    dir_entry.permissions[1] =
        static_cast<u8>((static_cast<u32>(perms) >> 3) & 0b111); // group
    dir_entry.permissions[2] =
        static_cast<u8>((static_cast<u32>(perms)) & 0b111); // others

    files.push_back(std::move(dir_entry));
    ArchiveFile *result = &files.back();

    // Recursively add children
    // "put them in the juvenile detention center" - 🤓
    try {
        for (const auto &entry : fs::directory_iterator(normalized_path)) {
            const string entry_path = entry.path().string();

            if (entry.is_directory()) {
                add_directory(entry_path);
            } else {
                add_file(entry_path);
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

    // Figure out parent directories so we can add those first
    // Just the entry for parent directories
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
#ifdef __x86_64__
    header_copy.crc32 = crc32_simd(data.data(), data.size());
#else
    header_copy.crc32 = crc32(data.data(), data.size());
#endif

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
#ifdef __x86_64__
    header_copy.crc32 = crc32_simd(data.data(), data.size());
#else
    header_copy.crc32 = crc32(data.data(), data.size());
#endif

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
    out.seekp((long)data_start_offset + static_cast<std::streamoff>(data_size) -
              1);
    out.write("", 1);
    out.close();

    // Now write data section in parallel using thread pool
    if (data_size > 0) {
        // Resize thread pool if needed
        if (thread_pool.size() != num_threads) {
            thread_pool.resize(num_threads);
        }

        atomic<size_t> current_chunk{0};
        mutex error_mutex;
        bool has_error = false;
        string error_message;

        // Calculate optimal chunk size for parallel processing
        const size_t min_chunk_size =
            256UL * 1024UL; // Increased to 256KB minimum
        const size_t max_chunk_size = 8UL * 1024UL * 1024UL; // 8MB maximum

        // Calculate chunk size based on data size and thread count
        size_t optimal_chunk_size = data_size / (num_threads * 2);
        optimal_chunk_size = std::max(
            min_chunk_size, std::min(max_chunk_size, optimal_chunk_size));

        const size_t chunk_size = optimal_chunk_size;
        const size_t total_chunks = (data_size + chunk_size - 1) / chunk_size;

        // Pre-open file descriptors for each thread to reduce overhead
        vector<ofstream> thread_files(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            thread_files[i].open(output_path, ios::binary | ios::in | ios::out);
            if (!thread_files[i]) {
                cerr << "Failed to open file for thread " << i << ": "
                     << output_path << '\n';
                return;
            }
        }

        // Task function for thread pool
        auto write_chunk_task = [&](size_t thread_id) {
            ofstream &thread_file = thread_files[thread_id];

            while (true) {
                size_t chunk_idx = current_chunk.fetch_add(1);
                if (chunk_idx >= total_chunks)
                    break;

                size_t chunk_start = chunk_idx * chunk_size;
                size_t chunk_end =
                    std::min(chunk_start + chunk_size, data_size);
                size_t actual_chunk_size = chunk_end - chunk_start;

                try {
                    // Seek to the correct position in the data section
                    thread_file.seekp(static_cast<streamsize>(
                        data_start_offset + chunk_start));

                    // Write this chunk of data
                    thread_file.write(
                        reinterpret_cast<const char *>(data.data() +
                                                       chunk_start),
                        static_cast<streamsize>(actual_chunk_size));

                    thread_file.flush(); // Ensure data is written

                    if (!thread_file.good()) {
                        lock_guard<mutex> lock(error_mutex);
                        has_error = true;
                        error_message = "Failed to write data chunk";
                        return;
                    }

                    if (verbose) {
                        lock_guard<mutex> lock(error_mutex);
                        cout << "Thread " << thread_id << " wrote chunk "
                             << chunk_idx + 1 << "/" << total_chunks << " ("
                             << actual_chunk_size << " bytes)" << '\n';
                    }
                } catch (const exception &e) {
                    lock_guard<mutex> lock(error_mutex);
                    has_error = true;
                    error_message = string("Error writing chunk: ") + e.what();
                    return;
                }
            }
        };

        // Submit tasks to thread pool
        vector<future<void>> futures;
        futures.reserve(num_threads);

        for (size_t i = 0; i < num_threads; ++i) {
            futures.push_back(thread_pool.enqueue(write_chunk_task, i));
        }

        // Wait for all tasks to complete
        for (auto &future : futures) {
            future.wait();
        }

        // Close all thread files
        for (auto &file : thread_files) {
            file.close();
        }

        // Check for errors
        if (has_error) {
            cerr << "Parallel compression failed: " << error_message << '\n';
            return;
        }

        if (verbose) {
            cout << "Parallel compression completed successfully using thread "
                    "pool with "
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

    // Worker function for extracting files using thread pool
    auto extract_task = [&](size_t start_idx, size_t end_idx) {
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

    // Resize thread pool if needed
    if (thread_pool.size() != num_threads) {
        thread_pool.resize(num_threads);
    }

    // Divide work among thread pool tasks
    std::vector<std::future<void>> futures;
    futures.reserve(num_threads);

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
            futures.push_back(thread_pool.enqueue(
                extract_task, start_idx, std::min(end_idx, files.size())));
        }
    }

    // Wait for all tasks to complete
    for (auto &future : futures) {
        future.wait();
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

    // Read data size first
    u64 data_size = 0;
    file.seekg(static_cast<streamoff>(data_section_offset - sizeof(u64)));
    file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));

    // Optimize memory allocation with reserve
    data.clear();
    data.reserve(static_cast<size_t>(data_size));
    data.resize(static_cast<size_t>(data_size));

    // Seek to data section and read with larger buffer for better I/O
    // performance
    file.seekg(static_cast<streamoff>(data_section_offset));

    constexpr size_t READ_BUFFER_SIZE = 2UL * 1024UL * 1024UL; // 2MB buffer
    if (data_size <= READ_BUFFER_SIZE) {
        // Small data: read in one go
        file.read(reinterpret_cast<char *>(data.data()),
                  static_cast<streamsize>(data_size));
    } else {
        // Large data: read in chunks
        size_t remaining = static_cast<size_t>(data_size);
        size_t offset = 0;

        while (remaining > 0) {
            size_t to_read = std::min(remaining, READ_BUFFER_SIZE);
            file.read(reinterpret_cast<char *>(data.data() + offset),
                      static_cast<streamsize>(to_read));

            auto bytes_read = file.gcount();
            if (bytes_read <= 0)
                break;

            offset += static_cast<size_t>(bytes_read);
            remaining -= static_cast<size_t>(bytes_read);
        }
    }

    // Validate CRC32 now that we have the data
#ifdef __x86_64__
    u32 actual_crc = crc32_simd(data.data(), static_cast<size_t>(data.size()));
#else
    u32 actual_crc = crc32(data.data(), static_cast<size_t>(data.size()));
#endif
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

        // Optimize buffer allocation using memory pool for large files
        const size_t file_size = static_cast<size_t>(file.data_length);
        std::vector<u8> file_data;

        if (file_size > 1024UL * 1024UL) { // Use memory pool for files > 1MB
            file_data = memory_pool.get_buffer(file_size);
            file_data.resize(file_size);
        } else {
            file_data.resize(file_size);
        }

        // Seek to the file's data position
        u64 absolute_offset = data_section_offset + file.offset;
        archive_file.seekg(static_cast<streamoff>(absolute_offset));

        // Read with optimized I/O for large files
        if (file_size <= 64UL * 1024UL) { // 64KB threshold
            // Small files: single read
            archive_file.read(reinterpret_cast<char *>(file_data.data()),
                              static_cast<streamsize>(file_size));
        } else {
            // Large files: chunked read for better I/O performance
            constexpr size_t CHUNK_SIZE = 64UL * 1024UL; // 64KB chunks
            size_t remaining = file_size;
            size_t offset = 0;

            while (remaining > 0) {
                size_t to_read = std::min(remaining, CHUNK_SIZE);
                archive_file.read(
                    reinterpret_cast<char *>(file_data.data() + offset),
                    static_cast<streamsize>(to_read));

                auto bytes_read = archive_file.gcount();
                if (bytes_read <= 0)
                    break;

                offset += static_cast<size_t>(bytes_read);
                remaining -= static_cast<size_t>(bytes_read);
            }
        }

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

        // Use more efficient memory copy for large files
        const size_t file_size = static_cast<size_t>(file.data_length);
        std::vector<u8> file_data;

        if (file_size > 1024UL * 1024UL) {
            file_data = memory_pool.get_buffer(file_size);
            file_data.resize(file_size);
        } else {
            file_data.resize(file_size);
        }

        // Use optimized memory copy for better performance on large files
        if (file_size > 0) {
            fast_memcpy(file_data.data(), data.data() + file.offset, file_size);
        }

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
