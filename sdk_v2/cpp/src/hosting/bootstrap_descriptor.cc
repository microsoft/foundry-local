// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/bootstrap_descriptor.h"

#include "exception.h"
#include "util/sha256.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
constexpr uintmax_t kMaximumDescriptorBytes = 1024 * 1024;
constexpr uint32_t kBootstrapProtocol = 1;

std::string PathText(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return {utf8.begin(), utf8.end()};
}

std::filesystem::path Utf8Path(const std::string& text) {
  if (text.find('\0') != std::string::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host bootstrap path contains a null character");
  }

  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::filesystem::path PathValue(const Json& value) {
  const auto text = value.get<std::string>();
  const auto path = Utf8Path(text);
  if (!path.is_absolute()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host bootstrap requires absolute artifact paths");
  }

  return path;
}

std::string HashValue(const Json& value) {
  auto hash = value.get<std::string>();
  if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
      })) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host bootstrap requires SHA256 artifact identities");
  }

  for (auto& c : hash) {
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }

  return hash;
}

Json EncodeArtifact(const BootstrapArtifact& artifact) {
  return {{"path", PathText(artifact.path)}, {"sha256", artifact.sha256}};
}

BootstrapArtifact DecodeArtifact(const Json& value) {
  return {PathValue(value.at("path")), HashValue(value.at("sha256"))};
}

void VerifyFile(const BootstrapArtifact& artifact) {
  if (!std::filesystem::is_regular_file(artifact.path) || Sha256File(artifact.path) != artifact.sha256) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host artifact is missing or changed: ", artifact.path);
  }
}

Configuration DecodeConfiguration(const Json& value, const Json& profile) {
  Configuration configuration;
  configuration.app_name = value.at("app_name").get<std::string>();
  if (configuration.app_name.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host bootstrap requires an application name");
  }

  configuration.app_data_dir = PathText(PathValue(profile.at("app_data_dir")));
  configuration.model_cache_dir = PathText(PathValue(profile.at("model_cache_dir")));
  configuration.logs_dir = PathText(PathValue(profile.at("logs_dir")));
  const auto level = profile.at("log_level").get<int>();
  if (level < static_cast<int>(LogLevel::Verbose) || level > static_cast<int>(LogLevel::Fatal)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid native host bootstrap log level");
  }

  configuration.log_level = static_cast<LogLevel>(level);
  configuration.catalog_region = profile.at("catalog_region").get<std::string>();
  for (const auto& catalog : profile.at("catalogs")) {
    configuration.catalog_urls.emplace_back(
        catalog.at("url").get<std::string>(),
        catalog.at("filter").is_null() ? std::nullopt : std::optional{catalog.at("filter").get<std::string>()});
  }

  configuration.web_service_endpoints = profile.at("http_endpoints").get<std::vector<std::string>>();
  configuration.additional_options = profile.at("execution_options").get<std::map<std::string, std::string>>();
  configuration.disable_nonessential_telemetry = profile.at("disable_nonessential_telemetry").get<bool>();
  if (configuration.additional_options.contains(kChildHostOption)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host bootstrap contains a nested child role");
  }

  return configuration;
}

}  // namespace

Json EncodeBootstrapDescriptor(const BootstrapDescriptor& descriptor) {
  return {{"version", kBootstrapProtocol},
          {"native", EncodeArtifact(descriptor.native)},
          {"ort", EncodeArtifact(descriptor.ort)},
          {"genai", EncodeArtifact(descriptor.genai)},
          {"manifest", PathText(descriptor.manifest)},
          {"native_build", descriptor.native_build},
          {"rid", descriptor.runtime_identifier},
          {"compatibility", descriptor.compatibility},
          {"app_name", descriptor.configuration.app_name},
          {"child", Json::parse(EncodeChildHostRole(descriptor.child))}};
}

BootstrapDescriptor DecodeBootstrapDescriptor(const Json& value) {
  try {
    if (value.at("version") != kBootstrapProtocol) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported native host bootstrap version");
    }

    BootstrapDescriptor descriptor;
    descriptor.native = DecodeArtifact(value.at("native"));
    descriptor.ort = DecodeArtifact(value.at("ort"));
    descriptor.genai = DecodeArtifact(value.at("genai"));
    descriptor.manifest = PathValue(value.at("manifest"));
    descriptor.native_build = value.at("native_build").get<std::string>();
    descriptor.runtime_identifier = value.at("rid").get<std::string>();
    descriptor.compatibility = value.at("compatibility");
    descriptor.child = DecodeChildHostRole(value.at("child").dump());
    const auto& runtime = descriptor.compatibility.at("runtime");
    if (descriptor.native_build.empty() || runtime.at("native_build") != descriptor.native_build ||
        runtime.at("rid") != descriptor.runtime_identifier || runtime.at("protocol") != kBootstrapProtocol ||
        Sha256String(descriptor.compatibility.dump()) != descriptor.child.profile_key) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host bootstrap does not match the selected profile");
    }

    descriptor.configuration = DecodeConfiguration(value, descriptor.compatibility.at("profile"));
    return descriptor;
  } catch (const Json::exception&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid native host bootstrap descriptor");
  }
}

Json ReadBootstrapJson(const std::filesystem::path& path) {
  if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > kMaximumDescriptorBytes) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host bootstrap file is missing or too large");
  }

  std::ifstream input(path, std::ios::binary);
  std::string bytes;
  bytes.resize(static_cast<size_t>(kMaximumDescriptorBytes + 1));
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input.eof() || input.bad() || static_cast<size_t>(input.gcount()) > kMaximumDescriptorBytes) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "cannot read native host bootstrap file");
  }

  bytes.resize(static_cast<size_t>(input.gcount()));
  return Json::parse(bytes);
}

void VerifyBootstrapArtifacts(const BootstrapDescriptor& descriptor, const std::filesystem::path& executable,
                              std::string_view expected_build, std::string_view expected_rid) {
  const auto manifest = ReadBootstrapJson(descriptor.manifest);
  if (manifest.at("version") != kBootstrapProtocol || manifest.at("protocol") != kBootstrapProtocol ||
      manifest.at("native_build").get<std::string>() != expected_build ||
      manifest.at("rid").get<std::string>() != expected_rid || descriptor.native_build != expected_build ||
      descriptor.runtime_identifier != expected_rid) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host executable/runtime release is incompatible");
  }

  const auto directory = std::filesystem::canonical(descriptor.manifest).parent_path();
  const auto& native = manifest.at("native");
  const auto& host = manifest.at("host");
  const auto native_name = Utf8Path(native.at("file_name").get<std::string>());
  const auto host_name = Utf8Path(host.at("file_name").get<std::string>());
  if (native_name.has_parent_path() || host_name.has_parent_path() || native_name.empty() || host_name.empty() ||
      std::filesystem::canonical(descriptor.native.path) != std::filesystem::canonical(directory / native_name) ||
      std::filesystem::canonical(executable) != std::filesystem::canonical(directory / host_name) ||
      HashValue(native.at("sha256")) != descriptor.native.sha256) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host assets do not match their runtime manifest");
  }

  VerifyFile({executable, HashValue(host.at("sha256"))});
  VerifyFile(descriptor.native);
  VerifyFile(descriptor.ort);
  VerifyFile(descriptor.genai);
}

}  // namespace fl::hosting
