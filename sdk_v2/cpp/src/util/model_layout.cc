// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/model_layout.h"

#include <system_error>

namespace fl {

namespace fs = std::filesystem;

namespace {

enum class FileProbe {
  Absent,
  RegularFile,
  Other,
  Error,
};

FileProbe ProbeRegularFile(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) {
    return FileProbe::Absent;
  }

  if (ec) {
    return FileProbe::Error;
  }

  if (!fs::exists(status)) {
    return FileProbe::Absent;
  }

  return fs::is_regular_file(status) ? FileProbe::RegularFile : FileProbe::Other;
}

}  // anonymous namespace

ModelLayout ClassifyModelLayout(const fs::path& model_path) {
  std::error_code ec;
  const auto root_status = fs::status(model_path, ec);
  if (ec || !fs::is_directory(root_status)) {
    return ModelLayout::Invalid;
  }

  const auto genai_config = ProbeRegularFile(model_path / "genai_config.json");
  if (genai_config == FileProbe::Error || genai_config == FileProbe::Other) {
    return ModelLayout::Invalid;
  }

  if (genai_config == FileProbe::RegularFile) {
    return ModelLayout::FlatModel;
  }

  const auto manifest = ProbeRegularFile(model_path / "manifest.json");
  if (manifest == FileProbe::Error || manifest == FileProbe::Other) {
    return ModelLayout::Invalid;
  }

  if (manifest == FileProbe::RegularFile) {
    return ModelLayout::ModelPackage;
  }

  return ModelLayout::Incomplete;
}

}  // namespace fl
