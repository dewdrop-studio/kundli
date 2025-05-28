#include "kundli.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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
  bool quiet{false};

  enum class Operation {
    None,
    Help,
    Version,
    Compress,
    Decompress,
    List
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
      } else if (arg == "-v" || arg == "--verbose") {
        verbose = true;
      } else if (arg == "-q" || arg == "--quiet") {
        quiet = true;
      } else if (arg == "-h" || arg == "--help") {
        operation = Operation::Help;
      } else if (arg == "-V" || arg == "--version") {
        operation = Operation::Version;
      } else if (arg == "-a" || arg == "--archive") {
        if (i + 1 < argc) {
          archive_path = argv[++i];
        } else {
          fprintf(stderr, "Error: --archive requires a path argument.\n");
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
    printf("  -v, --verbose         Enable verbose output\n");
    printf("  -q, --quiet           Suppress output messages\n");
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
        fprintf(stderr, "Error: Archive path is required for compression.\n");
        printHelp();
        std::exit(EXIT_FAILURE);
      }
      if (files.empty()) {
        fprintf(stderr, "Error: No files specified for compression.\n");
        printHelp();
        std::exit(EXIT_FAILURE);
      }
      archive = Archive::create();
      for (const auto &file : files) {
        if (!archive->add_file(file)) {
          fprintf(stderr, "Error: Failed to add file '%s' to archive.\n",
                  file.c_str());
          std::exit(EXIT_FAILURE);
        }
      }
      archive->compress(archive_path);
      break;

    case Operation::Decompress:
      archive = Archive::load(archive_path);
      if (!archive) {
        fprintf(stderr, "Error: Failed to load archive '%s'.\n",
                archive_path.c_str());
        std::exit(EXIT_FAILURE);
      }
      break;

    case Operation::List:
      archive = Archive::load(archive_path);
      if (!archive) {
        fprintf(stderr, "Error: Failed to load archive '%s'.\n",
                archive_path.c_str());
        std::exit(EXIT_FAILURE);
      }
      archive->list_files();
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
