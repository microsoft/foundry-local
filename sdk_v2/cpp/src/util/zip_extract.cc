// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/zip_extract.h"

#include "logger.h"

#include <archive.h>
#include <archive_entry.h>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fl {

bool IsSafeArchiveEntry(std::string_view entry) {
  if (entry.empty()) {
    return true;
  }

  if (entry.front() == '/' || entry.front() == '\\' || entry.find(':') != std::string_view::npos) {
    return false;
  }

  size_t start = 0;
  for (size_t i = 0; i <= entry.size(); ++i) {
    if (i != entry.size() && entry[i] != '/' && entry[i] != '\\') {
      continue;
    }

    if (entry.substr(start, i - start) == "..") {
      return false;
    }
    start = i + 1;
  }

  return true;
}

namespace {

struct ArchiveDeleter {
  void operator()(archive* value) const {
    if (value) {
      archive_read_free(value);
    }
  }
};

using ArchivePtr = std::unique_ptr<archive, ArchiveDeleter>;

enum class EntryKind { Regular, Directory };

uint16_t ReadU16(const uint8_t* data) { return static_cast<uint16_t>(data[0] | (data[1] << 8)); }

uint32_t ReadU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool ValidateZipStructure(const std::filesystem::path& zip_path, ILogger& logger) {
  constexpr uint32_t eocd_signature = 0x06054b50;
  constexpr uint32_t central_directory_signature = 0x02014b50;
  constexpr uint64_t eocd_size = 22;
  constexpr uint64_t central_header_size = 46;
  constexpr uint64_t max_comment_size = 65535;

  std::error_code ec;
  const auto file_size = std::filesystem::file_size(zip_path, ec);
  if (ec || file_size < eocd_size) {
    return false;
  }

  std::ifstream input(zip_path, std::ios::binary);
  const auto tail_size = std::min<uint64_t>(file_size, eocd_size + max_comment_size);
  std::vector<uint8_t> tail(static_cast<size_t>(tail_size));
  input.seekg(static_cast<std::streamoff>(file_size - tail_size));
  input.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
  if (!input) {
    return false;
  }

  size_t eocd_offset = std::string::npos;
  for (size_t i = tail.size() - static_cast<size_t>(eocd_size) + 1; i-- > 0;) {
    if (ReadU32(tail.data() + i) == eocd_signature) {
      const auto comment_size = ReadU16(tail.data() + i + 20);
      if (i + eocd_size + comment_size == tail.size()) {
        eocd_offset = i;
        break;
      }
    }
  }

  if (eocd_offset == std::string::npos) {
    logger.Log(LogLevel::Warning, "ExtractZip: invalid end-of-central-directory record");
    return false;
  }

  const auto* eocd = tail.data() + eocd_offset;
  const auto disk = ReadU16(eocd + 4);
  const auto central_disk = ReadU16(eocd + 6);
  const auto entries_on_disk = ReadU16(eocd + 8);
  const auto total_entries = ReadU16(eocd + 10);
  const uint64_t central_size = ReadU32(eocd + 12);
  const uint64_t central_offset = ReadU32(eocd + 16);
  const uint64_t absolute_eocd_offset = file_size - tail_size + eocd_offset;

  if (disk != 0 || central_disk != 0 || entries_on_disk != total_entries || total_entries == 0xffff ||
      central_size == 0xffffffff || central_offset == 0xffffffff || central_offset > absolute_eocd_offset ||
      central_size != absolute_eocd_offset - central_offset) {
    logger.Log(LogLevel::Warning, "ExtractZip: invalid central-directory bounds");
    return false;
  }

  input.clear();
  input.seekg(static_cast<std::streamoff>(central_offset));
  uint64_t central_bytes_read = 0;
  for (uint16_t i = 0; i < total_entries; ++i) {
    uint8_t header[central_header_size];
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!input || ReadU32(header) != central_directory_signature) {
      logger.Log(LogLevel::Warning, "ExtractZip: invalid central-directory signature");
      return false;
    }

    const auto compression_method = ReadU16(header + 10);
    if (compression_method != 0 && compression_method != 8) {
      logger.Log(LogLevel::Warning,
                 fmt::format("ExtractZip: unsupported compression method {}", compression_method));
      return false;
    }

    const uint64_t variable_size =
        static_cast<uint64_t>(ReadU16(header + 28)) + ReadU16(header + 30) + ReadU16(header + 32);
    const uint64_t record_size = central_header_size + variable_size;
    if (record_size > central_size - central_bytes_read) {
      logger.Log(LogLevel::Warning, "ExtractZip: invalid central-directory entry bounds");
      return false;
    }

    input.seekg(static_cast<std::streamoff>(variable_size), std::ios::cur);
    if (!input) {
      logger.Log(LogLevel::Warning, "ExtractZip: invalid central-directory entry");
      return false;
    }

    central_bytes_read += record_size;
  }

  if (central_bytes_read != central_size) {
    logger.Log(LogLevel::Warning, "ExtractZip: central-directory entry count does not match its size");
    return false;
  }

  return true;
}

std::string NormalizeEntryName(std::string_view name) {
  std::string normalized(name);
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }
  return std::filesystem::path(normalized).lexically_normal().generic_string();
}

std::string ComparisonKey(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool IsPortableArchivePath(std::string_view value) {
  for (const auto& component_path : std::filesystem::path(value)) {
    const auto component = component_path.generic_string();
    if (component.empty() || component.back() == ' ' || component.back() == '.') {
      return false;
    }

    if (std::any_of(component.begin(), component.end(), [](unsigned char ch) {
          return ch < 32 || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*';
        })) {
      return false;
    }

    auto device_name = component.substr(0, component.find('.'));
    std::transform(device_name.begin(), device_name.end(), device_name.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (device_name == "CON" || device_name == "PRN" || device_name == "AUX" || device_name == "NUL" ||
        (device_name.size() == 4 && (device_name.starts_with("COM") || device_name.starts_with("LPT")) &&
         device_name[3] >= '1' && device_name[3] <= '9')) {
      return false;
    }
  }

  return true;
}

ArchivePtr OpenArchive(const std::filesystem::path& zip_path, ILogger& logger) {
  ArchivePtr reader(archive_read_new());
  if (!reader) {
    logger.Log(LogLevel::Warning, "ExtractZip: failed to create archive reader");
    return nullptr;
  }

  archive_read_support_format_zip(reader.get());
#ifdef _WIN32
  const auto open_result = archive_read_open_filename_w(reader.get(), zip_path.c_str(), 64 * 1024);
#else
  const auto open_result = archive_read_open_filename(reader.get(), zip_path.c_str(), 64 * 1024);
#endif
  if (open_result != ARCHIVE_OK) {
    logger.Log(LogLevel::Warning, fmt::format("ExtractZip: failed to open '{}': {}", zip_path.string(),
                                              archive_error_string(reader.get())));
    return nullptr;
  }

  return reader;
}

bool EnsureActualDirectory(const std::filesystem::path& path) {
  if (path.empty()) {
    return true;
  }

  const auto parent = path.parent_path();
  if (parent != path && !EnsureActualDirectory(parent)) {
    return false;
  }

  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  if (ec && status.type() != std::filesystem::file_type::not_found) {
    return false;
  }

  if (std::filesystem::is_directory(status)) {
    return true;
  }

  if (status.type() != std::filesystem::file_type::not_found) {
    return false;
  }

  ec.clear();
  std::filesystem::create_directory(path, ec);
  if (ec) {
    return false;
  }

  status = std::filesystem::symlink_status(path, ec);
  return !ec && std::filesystem::is_directory(status);
}

bool PrepareDestination(const std::filesystem::path& destination, ILogger& logger) {
  if (!EnsureActualDirectory(destination)) {
    logger.Log(LogLevel::Warning,
               fmt::format("ExtractZip: destination is not a safe directory: '{}'", destination.string()));
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::is_empty(destination, ec) || ec) {
    logger.Log(LogLevel::Warning, fmt::format("ExtractZip: destination is not empty: '{}'", destination.string()));
    return false;
  }

  return true;
}

bool ValidateArchive(const std::filesystem::path& zip_path, const ZipExtractLimits& limits, ILogger& logger) {
  auto reader = OpenArchive(zip_path, logger);
  if (!reader) {
    return false;
  }

  std::unordered_map<std::string, EntryKind> entries;
  uint64_t total_size = 0;
  size_t entry_count = 0;
  archive_entry* entry = nullptr;

  int result = ARCHIVE_OK;
  while ((result = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
    if (++entry_count > limits.max_entries) {
      logger.Log(LogLevel::Warning, "ExtractZip: archive exceeds the entry-count limit");
      return false;
    }

    const char* raw_name = archive_entry_pathname_utf8(entry);
    const std::string_view name = raw_name ? raw_name : "";
    if (!IsSafeArchiveEntry(name)) {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: unsafe archive entry '{}'", name));
      return false;
    }

    const auto normalized = NormalizeEntryName(name);
    if (normalized.empty() || normalized == "." || !IsSafeArchiveEntry(normalized) ||
        !IsPortableArchivePath(normalized)) {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: invalid archive entry '{}'", name));
      return false;
    }

    EntryKind kind;
    const auto file_type = archive_entry_filetype(entry);
    if (file_type == AE_IFREG) {
      kind = EntryKind::Regular;
    } else if (file_type == AE_IFDIR) {
      kind = EntryKind::Directory;
    } else {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: unsupported archive entry '{}'", name));
      return false;
    }

    if (archive_entry_is_encrypted(entry) == 1) {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: encrypted archive entry '{}'", name));
      return false;
    }

    const auto size = archive_entry_size(entry);
    if (size < 0 || static_cast<uint64_t>(size) > limits.max_entry_uncompressed_bytes) {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: archive entry '{}' exceeds the size limit", name));
      return false;
    }

    total_size += static_cast<uint64_t>(size);
    if (total_size > limits.max_total_uncompressed_bytes) {
      logger.Log(LogLevel::Warning, "ExtractZip: archive exceeds the total size limit");
      return false;
    }

    const auto key = ComparisonKey(normalized);
    if (!entries.emplace(key, kind).second) {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: duplicate archive entry '{}'", name));
      return false;
    }
  }

  if (result != ARCHIVE_EOF) {
    logger.Log(LogLevel::Warning,
               fmt::format("ExtractZip: failed reading archive: {}", archive_error_string(reader.get())));
    return false;
  }

  for (const auto& [path, kind] : entries) {
    auto parent = std::filesystem::path(path).parent_path();
    while (!parent.empty()) {
      auto it = entries.find(ComparisonKey(parent.generic_string()));
      if (it != entries.end() && it->second == EntryKind::Regular) {
        logger.Log(LogLevel::Warning, fmt::format("ExtractZip: file-directory collision at '{}'", path));
        return false;
      }
      parent = parent.parent_path();
    }
  }

  return true;
}

bool ExtractArchive(const std::filesystem::path& zip_path, const std::filesystem::path& destination,
                    const ZipExtractLimits& limits, ILogger& logger) {
  auto reader = OpenArchive(zip_path, logger);
  if (!reader) {
    return false;
  }

  uint64_t total_written = 0;
  archive_entry* entry = nullptr;
  char buffer[64 * 1024];

  int result = ARCHIVE_OK;
  while ((result = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
    const char* raw_name = archive_entry_pathname_utf8(entry);
    if (!raw_name) {
      logger.Log(LogLevel::Warning, "ExtractZip: archive entry has no path");
      return false;
    }

    const std::string name = NormalizeEntryName(raw_name);
    const auto output_path = destination / name;

    if (archive_entry_filetype(entry) == AE_IFDIR) {
      if (!EnsureActualDirectory(output_path)) {
        logger.Log(LogLevel::Warning, fmt::format("ExtractZip: unsafe directory path '{}'", output_path.string()));
        return false;
      }

      continue;
    }

    if (!EnsureActualDirectory(output_path.parent_path())) {
      logger.Log(LogLevel::Warning,
                 fmt::format("ExtractZip: unsafe directory path '{}'", output_path.parent_path().string()));
      return false;
    }

    std::error_code ec;
    const auto output_status = std::filesystem::symlink_status(output_path, ec);
    if (output_status.type() != std::filesystem::file_type::not_found) {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: refusing pre-existing output '{}'", output_path.string()));
      return false;
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: failed to create '{}'", output_path.string()));
      return false;
    }

    uint64_t entry_written = 0;
    while (true) {
      const auto count = archive_read_data(reader.get(), buffer, sizeof(buffer));
      if (count == 0) {
        break;
      }
      if (count < 0) {
        logger.Log(LogLevel::Warning,
                   fmt::format("ExtractZip: failed extracting '{}': {}", name, archive_error_string(reader.get())));
        return false;
      }

      entry_written += static_cast<uint64_t>(count);
      total_written += static_cast<uint64_t>(count);
      if (entry_written > limits.max_entry_uncompressed_bytes || total_written > limits.max_total_uncompressed_bytes) {
        logger.Log(LogLevel::Warning, fmt::format("ExtractZip: extracted size limit exceeded by '{}'", name));
        return false;
      }

      output.write(buffer, count);
      if (!output) {
        logger.Log(LogLevel::Warning, fmt::format("ExtractZip: failed writing '{}'", output_path.string()));
        return false;
      }
    }

    output.close();
    if (!output) {
      logger.Log(LogLevel::Warning, fmt::format("ExtractZip: failed finalizing '{}'", output_path.string()));
      return false;
    }

    std::filesystem::permissions(output_path,
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read | std::filesystem::perms::others_read,
                                 std::filesystem::perm_options::replace, ec);
  }

  if (result != ARCHIVE_EOF) {
    logger.Log(LogLevel::Warning,
               fmt::format("ExtractZip: failed reading archive: {}", archive_error_string(reader.get())));
    return false;
  }

  return true;
}

}  // namespace

bool ExtractZip(const std::filesystem::path& zip_path, const std::filesystem::path& destination, ILogger& logger,
                const ZipExtractLimits& limits) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(zip_path, ec)) {
    logger.Log(LogLevel::Warning, fmt::format("ExtractZip: archive does not exist: '{}'", zip_path.string()));
    return false;
  }

  return ValidateZipStructure(zip_path, logger) && ValidateArchive(zip_path, limits, logger) &&
         PrepareDestination(destination, logger) && ExtractArchive(zip_path, destination, limits, logger);
}

}  // namespace fl
