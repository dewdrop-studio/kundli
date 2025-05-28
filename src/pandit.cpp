#include "kundli.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

constexpr const size_t DEFAULT_THREAD_COUNT = 4;

class Config {
  public:
    Config(int argc, char **argv) {
        parseArguments(argc, argv);
        execute();
    }

  private:
    std::unique_ptr<Archive> archive{Archive::create()};
    std::string archive_path{"comp.kl"};
    std::vector<std::string> files;
    bool verbose{false};
    bool force_full_load{false};
    bool use_parallel{false};
    size_t thread_count{DEFAULT_THREAD_COUNT};

    enum class Operation : uint8_t {
        None,
        Help,
        Info,
        Version,
        Compress,
        Decompress,
        List,
        Extend
    } operation{Operation::None};

    void parseArguments(int argc, char **argv) {
        for (int i = 1; i < argc; ++i) {
            std::string arg(argv[i]);

            if (arg == "-c" || arg == "--compress") {
                operation = Operation::Compress;
            } else if (arg == "-x" || arg == "--extract") {
                operation = Operation::Decompress;
            } else if (arg == "-l" || arg == "--list") {
                operation = Operation::List;
            } else if (arg == "-e" || arg == "--extend") {
                operation = Operation::Extend;
            } else if (arg == "-v" || arg == "--verbose") {
                verbose = true;
            } else if (arg == "--full-load") {
                force_full_load = true;
            } else if (arg == "-j" || arg == "--parallel") {
                use_parallel = true;
            } else if (arg == "-t" || arg == "--threads") {
                if (i + 1 < argc) {
                    thread_count = std::stoul(argv[++i]);
                    use_parallel = true; // Enable parallel processing when
                                         // threads specified
                } else {
                    std::cerr
                        << "Error: --threads requires a number argument.\n";
                    std::exit(EXIT_FAILURE);
                }
            } else if (arg == "-i" || arg == "--info") {
                operation = Operation::Info;
            } else if (arg == "-h" || arg == "--help") {
                operation = Operation::Help;
            } else if (arg == "-V" || arg == "--version") {
                operation = Operation::Version;
            } else if (arg == "-a" || arg == "--archive") {
                if (i + 1 < argc) {
                    archive_path = argv[++i];
                } else {
                    std::cerr << "Error: --archive requires a path argument."
                              << "\n";
                    std::exit(EXIT_FAILURE);
                }
            } else {
                files.push_back(arg);
            }
        }
    }

    void printHelp() const {
        printf("Usage: pandit [options] [files...]\n");
        printf("Options:\n");
        printf("  -c, --compress        Compress files into an archive\n");
        printf("  -x, --extract         Extract files from an archive\n");
        printf("  -l, --list            List files in an archive\n");
        printf("  -e, --extend          Extend the archive with new files\n");
        printf("  -i, --info            Show archive information\n");
        printf("  -v, --verbose         Enable verbose output\n");
        printf("  -j, --parallel        Enable parallel processing\n");
        printf(
            "  -t, --threads N       Use N threads for parallel operations\n");
        printf("      --full-load       Force full loading (disable lazy "
               "loading)\n");
        printf("  -h, --help            Show this help message\n");
        printf("  -V, --version         Show version information\n");
        printf("  -a, --archive <path>  Specify the archive path (default: "
               "comp.kl)\n");
    }

    void printVersion() const {
        printf("Pandit Archive Tool\n");
        printf("Built on: %s\n", __DATE__);
    }

    void execute() {
        switch (operation) {
        case Operation::Help:
            printHelp();
            break;

        case Operation::Version:
            printVersion();
            break;

        case Operation::Compress:
            if (archive_path.empty()) {
                fprintf(stderr,
                        "Error: Archive path is required for compression.\n");
                printHelp();
                std::exit(EXIT_FAILURE);
            }
            if (files.empty()) {
                fprintf(stderr, "Error: No files specified for compression.\n");
                printHelp();
                std::exit(EXIT_FAILURE);
            }
            archive = Archive::create();
            archive->set_verbose(verbose);
            if (thread_count > 0) {
                archive->set_thread_count(thread_count);
            }

            for (const auto &file : files) {
                if (!archive->add_file(file)) {
                    fprintf(stderr,
                            "Error: Failed to add file '%s' to archive.\n",
                            file.c_str());
                    std::exit(EXIT_FAILURE);
                }
            }

            if (use_parallel) {
                archive->compress_parallel(archive_path, thread_count);
            } else {
                archive->compress(archive_path);
            }
            break;

        case Operation::Decompress:
            archive = force_full_load ? Archive::load_full(archive_path)
                                      : Archive::load(archive_path);
            if (!archive) {
                fprintf(stderr, "Error: Failed to load archive '%s'.\n",
                        archive_path.c_str());
                std::exit(EXIT_FAILURE);
            }
            archive->set_verbose(verbose);
            if (thread_count > 0) {
                archive->set_thread_count(thread_count);
            }

            if (use_parallel) {
                archive->decompress_parallel(thread_count);
            } else {
                archive->decompress();
            }
            break;
        case Operation::Extend:
            archive = force_full_load ? Archive::load_full(archive_path)
                                      : Archive::load(archive_path);
            if (!archive) {
                fprintf(stderr, "Error: Failed to load archive '%s'.\n",
                        archive_path.c_str());
                std::exit(EXIT_FAILURE);
            }
            if (files.empty()) {
                fprintf(stderr, "Error: No files specified for extending.\n");
                printHelp();
                std::exit(EXIT_FAILURE);
            }
            archive->set_verbose(verbose);
            if (thread_count > 0) {
                archive->set_thread_count(thread_count);
            }
            for (const auto &file : files) {
                if (!archive->add_file(file)) {
                    fprintf(stderr,
                            "Error: Failed to add file '%s' to archive.\n",
                            file.c_str());
                    std::exit(EXIT_FAILURE);
                }
            }

            if (use_parallel) {
                archive->compress_parallel(archive_path, thread_count);
            } else {
                archive->compress(archive_path);
            }
            break;

        case Operation::List:
            archive = force_full_load ? Archive::load_full(archive_path)
                                      : Archive::load(archive_path);
            if (!archive) {
                fprintf(stderr, "Error: Failed to load archive '%s'.\n",
                        archive_path.c_str());
                std::exit(EXIT_FAILURE);
            }
            archive->list_files();
            break;
        case Operation::Info:
            archive = force_full_load ? Archive::load_full(archive_path)
                                      : Archive::load(archive_path);
            if (!archive) {
                fprintf(stderr, "Error: Failed to load archive '%s'.\n",
                        archive_path.c_str());
                std::exit(EXIT_FAILURE);
            }
            archive->print_info();
            break;

        case Operation::None:
        default:
            fprintf(stderr, "Error: No operation specified.\n");
            printHelp();
            std::exit(EXIT_FAILURE);
        }

        std::exit(EXIT_SUCCESS);
    }
};

int main(int argc, char **argv) {
    Config config(argc, argv);
    return 0;
}
