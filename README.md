# Kundli Archive Tool

Kundli is a modern, efficient archive tool written in C++23 that allows you to create, extract, and manage compressed archives with the `.kl` format. It provides functionality similar to `tar` but with a custom binary format optimized for performance.

## Features

- **Create Archives**: Bundle multiple files and directories into a single `.kl` archive
- **Extract Archives**: Restore files from archives with original permissions and structure
- **List Contents**: Display archive contents in `ls -l` format with permissions and file types
- **Directory Support**: Recursive directory archiving with automatic parent directory creation
- **File Types**: Support for regular files, directories, and symbolic links
- **Permission Preservation**: Maintains original file permissions (owner, group, others)
- **Modern C++**: Built with C++23 features and best practices

## Installation

TODO!

### Prerequisites

- **C++ Compiler**: GCC 12+ or Clang 15+ with C++23 support
- **CMake**: Version 3.10 or higher
- **Build Tools**: `make` or equivalent build system for your platform

### Build Instructions

```bash
# Clone or navigate to the project directory
cd kundli

# Create and configure build directory
mkdir -p build
cd build
cmake ..

# Build the project
make

# The executable will be available as ./pandit
```

## Usage

The main executable is called `pandit` and provides several operations for archive management.

### Command Syntax

```bash
pandit [options] [files...]
```

### Options

| Option      | Long Form          | Description                               |
| ----------- | ------------------ | ----------------------------------------- |
| `-c`        | `--compress`       | Create a new archive from files           |
| `-x`        | `--extract`        | Extract files from an archive             |
| `-l`        | `--list`           | List contents of an archive               |
| `-a <path>` | `--archive <path>` | Specify archive path (default: `comp.kl`) |
| `-v`        | `--verbose`        | Enable verbose output                     |
| `-q`        | `--quiet`          | Suppress output messages                  |
| `-h`        | `--help`           | Show help message                         |
| `-V`        | `--version`        | Show version information                  |

### Examples

#### Creating Archives

```bash
# Create an archive with specific files
./pandit -c -a myarchive.kl file1.txt file2.txt directory/

# Create an archive with default name
./pandit -c file1.txt directory/

# Archive an entire directory structure
./pandit -c -a backup.kl /home/user/documents/
```

#### Listing Archive Contents

```bash
# List files in archive
./pandit -l -a myarchive.kl

# Example output:
# total 4
# drwxr-xr-x        0 documents
# -rw-r--r--     1024 documents/report.txt
# lrwxrwxrwx       10 documents/link.txt -> report.txt
```

#### Extracting Archives

```bash
# Extract all files from archive
./pandit -x -a myarchive.kl

# Extract from default archive
./pandit -x
```

## Archive Format

Kundli uses a custom binary format (`.kl`) with the following structure:

### Header Structure
```cpp
struct ArchiveHeader {
  u8 magic[4];     // "KNDL" magic bytes
  u8 version;      // Format version (currently 1)
  u8 flags;        // Archive flags
  u64 timestamp;   // Creation timestamp
} __attribute__((packed));
```

### File Entry Structure
- **Metadata**: Offset, size, permissions, file type
- **Path Information**: Path length and actual path string
- **Data Length**: Size of file content
- **File Types**: Regular files, directories, symbolic links

### Data Section
- Raw file contents stored sequentially
- Symbolic link targets stored as data
- Directories have no data content

## Programming API

The project provides a C++ API for programmatic archive manipulation:

```cpp
#include "kundli.hpp"

// Create a new archive
auto archive = Archive::create();

// Add files and directories
archive->add_file("path/to/file.txt");
archive->add_directory("path/to/directory/");

// Save the archive
archive->compress("output.kl");

// Load an existing archive
auto loaded_archive = Archive::load("existing.kl");
if (loaded_archive) {
    // List contents
    loaded_archive->list_files();
    
    // Extract files
    loaded_archive->decompress();
    
    // Get archive information
    loaded_archive->print_info();
}
```

### Key API Methods

| Method                | Description                            |
| --------------------- | -------------------------------------- |
| `Archive::create()`   | Create a new empty archive             |
| `Archive::load(path)` | Load archive from file                 |
| `add_file(path)`      | Add single file to archive             |
| `add_directory(path)` | Add directory and contents recursively |
| `remove_file(path)`   | Remove file from archive               |
| `compress(path)`      | Save archive to file                   |
| `decompress()`        | Extract all files to filesystem        |
| `list_files()`        | Display archive contents               |

## Project Structure

```
kundli/
├── CMakeLists.txt          # Build configuration
├── README.md               # This documentation
├── LICENSE                 # MIT License
├── include/
│   └── kundli.hpp         # Main header file with API
├── src/
│   ├── kundli.cpp         # Archive library implementation
│   └── pandit.cpp         # Command-line tool implementation
├── build/                 # Build output directory
└── test/                  # Test files and examples
```

## Advanced Features

### Automatic Parent Directory Creation

When adding files, Kundli automatically creates parent directory entries in the archive:

```bash
# Adding deep/nested/file.txt automatically creates:
# - deep/ (directory)
# - deep/nested/ (directory)  
# - deep/nested/file.txt (file)
```

### Permission Handling

File permissions are stored as 3-bit values for owner, group, and others:
- Read (r): 4
- Write (w): 2
- Execute (x): 1

### Symbolic Link Support

Symbolic links are preserved with their target paths stored in the archive data section.

## Development

### Code Style
- Modern C++23 features
- RAII and smart pointers
- Exception safety
- Type aliases for clarity (`u8`, `u16`, `u32`, `u64`)

### Building for Development

```bash
# Debug build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make

# Release build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

### Type System

The project uses convenient type aliases:
```cpp
using u8 = std::uint8_t;    // Unsigned 8-bit
using u16 = std::uint16_t;  // Unsigned 16-bit  
using u32 = std::uint32_t;  // Unsigned 32-bit
using u64 = std::uint64_t;  // Unsigned 64-bit

using s8 = std::int8_t;     // Signed 8-bit
using s16 = std::int16_t;   // Signed 16-bit
using s32 = std::int32_t;   // Signed 32-bit
using s64 = std::int64_t;   // Signed 64-bit
```

## Error Handling

The tool provides comprehensive error handling:
- File access validation
- Archive format verification
- Permission restoration errors
- Filesystem operation failures

## Performance Considerations

- Binary format for fast I/O
- Minimal memory allocations
- Efficient directory traversal
- Stream-based file processing

## Troubleshooting

### Common Issues

**Build Errors**
```bash
# Ensure C++23 support
g++ --version  # Should be 12+ 
clang++ --version  # Should be 15+
```

**Permission Issues**
```bash
# Make executable
chmod +x build/pandit

# Check file permissions
ls -la build/pandit
```

**Archive Corruption**
- Archive files are binary - avoid text mode transfers
- Verify archive with: `./pandit -l -a archive.kl`

### Getting Help

```bash
# Show usage information
./pandit --help

# Show version
./pandit --version

# List archive contents to debug
./pandit -l -a problematic.kl
```

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## Changelog

### Version 1.0.0
- Initial release
- Basic archive creation and extraction
- Directory support with recursive operations
- Symbolic link preservation
- Permission handling
- `ls -l` style listing format

---

**Kundli Archive Tool** - Modern C++23 archive solution by dewdrop studio