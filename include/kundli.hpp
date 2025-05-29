#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Memory mapping support for large files
#ifdef __unix__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

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
    static std::unique_ptr<Archive> load_full(const std::string &path);
    ~Archive();

    ArchiveFile *add_file(const std::string &path);
    ArchiveFile *add_directory(const std::string &path);

    void remove_file(const std::string &path);

    void compress(const std::string &output_path) const;
    void compress_parallel(const std::string &output_path,
                           size_t num_threads = 0) const;
    void decompress();
    void decompress_parallel(size_t num_threads = 0);
    void decompress_file(const std::string &file_path,
                         const std::string &output_path);

    void list_files() const;
    void print_info() const;

    void set_verbose(bool verbose) { this->verbose = verbose; }

    // Lazy loading methods
    const std::vector<u8> get_file_data(const ArchiveFile &file) const;
    const std::vector<u8> get_file_data(const std::string &file_path) const;
    bool is_loaded() const { return lazy_loaded; }

    // Memory mapping for large files
    class MappedFile {
      public:
        MappedFile() = default;
        ~MappedFile();

        bool map_file(const std::string &path);
        void unmap();

        const u8 *data() const { return mapped_data; }
        size_t size() const { return file_size; }
        bool is_mapped() const { return mapped_data != nullptr; }

      private:
#ifdef __unix__
        int fd = -1;
#endif
        u8 *mapped_data = nullptr;
        size_t file_size = 0;
    };

    // Threading configuration
    void set_thread_count(size_t count) { thread_count = count; }
    size_t get_thread_count() const { return thread_count; }

  private:
    Archive() = default;

    void add_parent_directories(const std::string &path);
    std::string normalize_path(const std::string &path);
    void load_file_data_if_needed();

    ArchiveHeader header{};
    std::vector<ArchiveFile> files;
    std::vector<u8> data;
    bool verbose{false};

    // Lazy loading support
    std::string archive_file_path;
    u64 data_section_offset{0};
    bool lazy_loaded{false};

    // Memory mapping for very large archives (>100MB)
    mutable MappedFile mapped_archive;
    static constexpr size_t MMAP_THRESHOLD = 100UL * 1024UL * 1024UL; // 100MB

    // Threading support
    size_t thread_count{0}; // 0 means auto-detect

    // Memory pool for better allocation performance
    struct MemoryPool {
        std::vector<std::vector<u8>> buffers;
        std::mutex pool_mutex;

        std::vector<u8> get_buffer(size_t size) {
            std::lock_guard<std::mutex> lock(pool_mutex);
            for (auto it = buffers.begin(); it != buffers.end(); ++it) {
                if (it->size() >= size && it->capacity() <= size * 2) {
                    auto result = std::move(*it);
                    buffers.erase(it);
                    return result;
                }
            }
            return std::vector<u8>(size);
        }

        void return_buffer(std::vector<u8> buffer) {
            std::lock_guard<std::mutex> lock(pool_mutex);
            if (buffers.size() < 10) { // Limit pool size
                buffer.clear();
                buffers.push_back(std::move(buffer));
            }
        }
    };

    static MemoryPool memory_pool;

    // Thread pool for better parallel processing
    class ThreadPool {
      public:
        ThreadPool(size_t threads = std::thread::hardware_concurrency());
        ~ThreadPool();

        template <class F, class... Args>
        auto enqueue(F &&f, Args &&...args)
            -> std::future<typename std::result_of<F(Args...)>::type> {
            using return_type = typename std::result_of<F(Args...)>::type;

            auto task = std::make_shared<std::packaged_task<return_type()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));

            std::future<return_type> res = task->get_future();
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                if (stop) {
                    throw std::runtime_error("enqueue on stopped ThreadPool");
                }
                tasks.emplace([task]() { (*task)(); });
            }
            condition.notify_one();
            return res;
        }

        void resize(size_t new_size);
        size_t size() const { return workers.size(); }

      private:
        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;

        std::mutex queue_mutex;
        std::condition_variable condition;
        std::atomic<bool> stop;
    };

    static ThreadPool thread_pool;
};
