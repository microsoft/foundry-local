// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Shared test helper for hand-assembling minimal ZIP archives (STORE and DEFLATE, with control
// over Unix mode bits) so extraction behavior can be tested without any external zip/tar tool.
#pragma once

#include "utils/temp_path.h"

#include <zlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fl::test {

namespace zip_builder_detail {

inline void AppendU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

inline void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

/// Raw-DEFLATEs `data` (no zlib/gzip framing) — matches what real zip archives store.
inline std::vector<uint8_t> RawDeflate(const std::vector<uint8_t>& data) {
  z_stream strm{};
  deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);

  std::vector<uint8_t> out(compressBound(static_cast<uLong>(data.size())) + 64);
  strm.next_in = const_cast<Bytef*>(data.data());
  strm.avail_in = static_cast<uInt>(data.size());
  strm.next_out = out.data();
  strm.avail_out = static_cast<uInt>(out.size());

  deflate(&strm, Z_FINISH);
  out.resize(out.size() - strm.avail_out);
  deflateEnd(&strm);
  return out;
}

}  // namespace zip_builder_detail

class ZipBuilder {
 public:
  /// @param compress  STORE (false) or DEFLATE (true).
  /// @param unix_mode  Packed into external_attrs high word when host_os == 3 (Unix); 0 to omit.
  void AddEntry(const std::string& name, const std::vector<uint8_t>& data, bool compress = false,
                uint32_t unix_mode = 0) {
    AddEntryWithCompressionMethod(name, data, compress ? 8 : 0, unix_mode);
  }

  void AddEntryWithCompressionMethod(const std::string& name, const std::vector<uint8_t>& data,
                                     uint16_t compression_method, uint32_t unix_mode = 0) {
    Entry entry;
    entry.name = name;
    entry.crc = crc32(0, data.data(), static_cast<uInt>(data.size()));
    entry.uncompressed_size = static_cast<uint32_t>(data.size());
    entry.compression_method = compression_method;
    entry.data = compression_method == 8 ? zip_builder_detail::RawDeflate(data) : data;
    entry.compressed_size = static_cast<uint32_t>(entry.data.size());
    entry.host_os = unix_mode != 0 ? 3 : 0;
    entry.external_attrs = unix_mode != 0 ? (unix_mode << 16) : 0;
    entries_.push_back(std::move(entry));
  }

  /// Adds an explicit directory entry (name should end with '/').
  void AddDirectory(const std::string& name) {
    Entry entry;
    entry.name = name;
    entries_.push_back(std::move(entry));
  }

  std::vector<uint8_t> Build() const {
    using namespace zip_builder_detail;
    std::vector<uint8_t> out;
    std::vector<uint32_t> local_offsets;

    for (const auto& entry : entries_) {
      local_offsets.push_back(static_cast<uint32_t>(out.size()));
      AppendU32(out, 0x04034b50);
      AppendU16(out, 20);  // version needed
      AppendU16(out, 0);   // flags
      AppendU16(out, entry.compression_method);
      AppendU16(out, 0);  // mod time
      AppendU16(out, 0);  // mod date
      AppendU32(out, entry.crc);
      AppendU32(out, entry.compressed_size);
      AppendU32(out, entry.uncompressed_size);
      AppendU16(out, static_cast<uint16_t>(entry.name.size()));
      AppendU16(out, 0);  // extra field length
      out.insert(out.end(), entry.name.begin(), entry.name.end());
      out.insert(out.end(), entry.data.begin(), entry.data.end());
    }

    const uint32_t cd_offset = static_cast<uint32_t>(out.size());

    for (size_t i = 0; i < entries_.size(); ++i) {
      const auto& entry = entries_[i];
      AppendU32(out, 0x02014b50);
      out.push_back(0);              // version made by (low byte)
      out.push_back(entry.host_os);  // version made by (high byte = host OS)
      AppendU16(out, 20);            // version needed
      AppendU16(out, 0);             // flags
      AppendU16(out, entry.compression_method);
      AppendU16(out, 0);  // mod time
      AppendU16(out, 0);  // mod date
      AppendU32(out, entry.crc);
      AppendU32(out, entry.compressed_size);
      AppendU32(out, entry.uncompressed_size);
      AppendU16(out, static_cast<uint16_t>(entry.name.size()));
      AppendU16(out, 0);  // extra field length
      AppendU16(out, 0);  // comment length
      AppendU16(out, 0);  // disk number start
      AppendU16(out, 0);  // internal attrs
      AppendU32(out, entry.external_attrs);
      AppendU32(out, local_offsets[i]);
      out.insert(out.end(), entry.name.begin(), entry.name.end());
    }

    const uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;

    AppendU32(out, 0x06054b50);
    AppendU16(out, 0);  // disk number
    AppendU16(out, 0);  // cd start disk
    AppendU16(out, static_cast<uint16_t>(entries_.size()));
    AppendU16(out, static_cast<uint16_t>(entries_.size()));
    AppendU32(out, cd_size);
    AppendU32(out, cd_offset);
    AppendU16(out, 0);  // comment length

    return out;
  }

  std::filesystem::path WriteToTempFile() const {
    auto path = MakeUniqueTempPath("fl_zip_builder_");
    auto bytes = Build();
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
  }

  void WriteToFile(const std::filesystem::path& path) const {
    auto bytes = Build();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

 private:
  struct Entry {
    std::string name;
    std::vector<uint8_t> data;
    uint32_t crc = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint16_t compression_method = 0;
    uint32_t external_attrs = 0;
    uint8_t host_os = 0;
  };

  std::vector<Entry> entries_;
};

}  // namespace fl::test
