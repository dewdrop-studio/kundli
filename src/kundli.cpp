#include "kundli.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace std;
namespace fs = std::filesystem;

unique_ptr<Archive> Archive::create() {
  auto archive = unique_ptr<Archive>(new Archive());
  strncpy(reinterpret_cast<char *>(archive->header.magic), ARCHIVE_MAGIC, 7);
  archive->header.version = ARCHIVE_VERSION;
  archive->header.flags = static_cast<u8>(ArchiveFlag::None);
  return archive;
}

unique_ptr<Archive> Archive::load(const string &path) {
  ifstream file(path, ios::binary);
  if (!file) {
    cerr << "Failed to open archive: " << path << '\n';
    return nullptr;
  }

  auto archive = create();
  file.read(reinterpret_cast<char *>(&archive->header), sizeof(ArchiveHeader));

  if (strncmp(reinterpret_cast<char *>(archive->header.magic), ARCHIVE_MAGIC,
              6) != 0 ||
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
    file.read(file_entry.path.data(), file_entry.path_length);

    archive->files.push_back(std::move(file_entry));
  }

  u64 data_size = 0;
  file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
  archive->data.resize(data_size);
  file.read(reinterpret_cast<char *>(archive->data.data()), data_size);

  return archive;
}

Archive::~Archive() = default;

ArchiveFile *Archive::add_file(const string &path) {
  if (!fs::exists(path)) {
    cerr << "File does not exist: " << path << '\n';
    return nullptr;
  }

  ArchiveFile file_entry;
  file_entry.path = path;
  file_entry.path_length = path.size();
  file_entry.data_length = fs::file_size(path);
  file_entry.offset = data.size();
  file_entry.size = file_entry.data_length + file_entry.path_length;

  auto perms = fs::status(path).permissions();
  file_entry.permissions[0] =
      static_cast<u8>((static_cast<u32>(perms) >> 6) & 0b111); // owner
  file_entry.permissions[1] =
      static_cast<u8>((static_cast<u32>(perms) >> 3) & 0b111); // group
  file_entry.permissions[2] =
      static_cast<u8>((static_cast<u32>(perms)) & 0b111); // others

  if (fs::is_directory(path))
    file_entry.type = FileType::Directory;
  else if (fs::is_symlink(path))
    file_entry.type = FileType::Symlink;
  else
    file_entry.type = FileType::Regular;

  ifstream file(path, ios::binary);
  if (!file) {
    cerr << "Failed to open: " << path << '\n';
    return nullptr;
  }

  vector<u8> buffer(file_entry.data_length);
  file.read(reinterpret_cast<char *>(buffer.data()), file_entry.data_length);
  data.insert(data.end(), buffer.begin(), buffer.end());

  files.push_back(std::move(file_entry));
  return &files.back();
}

void Archive::remove_file(const string &path) {
  auto it = std::find_if(files.begin(), files.end(),
                         [&](const ArchiveFile &f) { return f.path == path; });
  if (it != files.end()) {
    files.erase(it);
  } else {
    cerr << "File not found: " << path << '\n';
  }
}

void Archive::compress(const string &output_path) const {
  ofstream out(output_path, ios::binary);
  if (!out) {
    cerr << "Failed to open output: " << output_path << '\n';
    return;
  }

  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
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
    out.write(file_entry.path.c_str(), file_entry.path_length);
  }

  u64 data_size = data.size();
  out.write(reinterpret_cast<const char *>(&data_size), sizeof(data_size));
  out.write(reinterpret_cast<const char *>(data.data()), data_size);
}

void Archive::decompress(const string &input_path) {
  ifstream file(input_path, ios::binary);
  if (!file) {
    cerr << "Failed to open input: " << input_path << '\n';
    return;
  }

  file.read(reinterpret_cast<char *>(&header), sizeof(header));
  u64 file_count;
  file.read(reinterpret_cast<char *>(&file_count), sizeof(file_count));

  files.reserve(file_count);
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
    file.read(file_entry.path.data(), file_entry.path_length);

    files.push_back(std::move(file_entry));
  }

  u64 data_size;
  file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
  data.resize(data_size);
  file.read(reinterpret_cast<char *>(data.data()), data_size);
}

void Archive::list_files() const {
  cout << "Files in archive (" << files.size() << "):\n";
  for (const auto &f : files)
    cout << " - " << f.path << " (" << f.data_length << " bytes)\n";
}

void Archive::print_info() const {
  cout << "Archive Info:\n";
  cout << "Magic: " << string(reinterpret_cast<const char *>(header.magic), 7)
       << '\n';
  cout << "Version: " << static_cast<int>(header.version) << '\n';
  cout << "Flags: " << static_cast<int>(header.flags) << '\n';
  cout << "Files: " << files.size() << '\n';
  cout << "Data Size: " << data.size() << " bytes\n";
}
