// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/non_generative_package.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/execution_provider.h"
#include "util/path_safety.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>

namespace fl {
namespace {

namespace fs = std::filesystem;

std::string NormalizeProvider(std::string_view provider) {
  if (provider.empty()) return {};
  switch (EPUtils::StringtoEP(provider)) {
    case ExecutionProvider::kCPU:
      return {};
    case ExecutionProvider::kCUDA:
      return "cuda";
    default:
      break;
  }
  throw std::invalid_argument(
      "non-generative packages support only CPU and CUDA execution providers: " +
      std::string(provider));
}

fs::path ContainedRegularFile(const fs::path& canonical_root,
                              const std::string& relative, const char* field) {
  const fs::path relative_path(relative);
  if (relative_path.empty() || relative_path.is_absolute())
    throw std::invalid_argument(std::string(field) +
                                " must be package-relative");
  const auto candidate = canonical_root / relative_path;
  std::error_code ec;
  const auto canonical_candidate = fs::canonical(candidate, ec);
  if (ec || !fs::is_regular_file(canonical_candidate, ec)) {
    throw std::invalid_argument(std::string(field) + " does not name an existing file");
  }
  if (!IsPathWithinDirectory(canonical_candidate, canonical_root)) {
    throw std::invalid_argument(std::string(field) + " resolves outside the package root");
  }
  return canonical_candidate;
}

uint64_t MeasureContainedPackage(const fs::path& canonical_root) {
  uint64_t total{};
  std::error_code ec;
  fs::recursive_directory_iterator iterator(
      canonical_root, fs::directory_options::none, ec);
  const fs::recursive_directory_iterator end;
  if (ec) throw std::invalid_argument("cannot enumerate package");
  for (; iterator != end; iterator.increment(ec)) {
    if (ec) throw std::invalid_argument("cannot enumerate package");
    const auto canonical_entry = fs::canonical(iterator->path(), ec);
    if (ec || !IsPathWithinDirectory(canonical_entry, canonical_root)) {
      throw std::invalid_argument(
          "package entry resolves outside the package root");
    }
    if (!fs::is_regular_file(canonical_entry, ec)) {
      if (ec) throw std::invalid_argument("cannot inspect package entry");
      continue;
    }
    const auto bytes = fs::file_size(canonical_entry, ec);
    if (ec) throw std::invalid_argument("cannot inspect package file size");
    if (bytes > (std::numeric_limits<uint64_t>::max)() - total)
      throw std::invalid_argument("package byte size overflows uint64");
    total += bytes;
  }
  if (ec) throw std::invalid_argument("cannot enumerate package");
  return total;
}

}  // namespace

bool DeclaresNonGenerativePackage(const fs::path& package_root) {
  std::error_code ec;
  const auto manifest_status =
      fs::symlink_status(package_root / "component_manifest.json", ec);
  if (!ec && manifest_status.type() != fs::file_type::not_found)
    return true;

  const auto inference_path = package_root / "inference_model.json";
  if (!fs::is_regular_file(inference_path, ec)) return false;
  try {
    std::ifstream stream(inference_path);
    const auto inference = nlohmann::json::parse(stream);
    const auto task = inference.value("Task", std::string{});
    return inference.contains("ComponentManifest") ||
           task == "ranking" || task == "decision";
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

std::optional<NonGenerativePackageMetadata> ReadNonGenerativePackage(
    const fs::path& package_root, bool allow_in_progress) {
  std::error_code ec;
  const auto canonical_root = fs::canonical(package_root, ec);
  if (ec || !fs::is_directory(canonical_root, ec)) return std::nullopt;
  if (!DeclaresNonGenerativePackage(canonical_root)) return std::nullopt;
  if (!allow_in_progress && fs::exists(canonical_root / "download.tmp")) {
    throw std::invalid_argument("package contains download.tmp");
  }

  NonGenerativePackageMetadata result;
  const auto inference_path = canonical_root / "inference_model.json";
  if (fs::is_regular_file(inference_path, ec)) {
    try {
      std::ifstream stream(inference_path);
      const auto inference = nlohmann::json::parse(stream);
      result.model_id = inference.value("Name", std::string{});
      result.alias = inference.value("Alias", std::string{});
      result.task = inference.value("Task", std::string{});
    } catch (const nlohmann::json::exception&) {
      // Discovery metadata remains owned by the existing catalog writer.
    }
  }
  const auto component_manifest =
      ContainedRegularFile(canonical_root, "component_manifest.json",
                           "component_manifest.json");
  const auto config_path =
      ContainedRegularFile(canonical_root, "genai_config.json",
                           "genai_config.json");
  ContainedRegularFile(canonical_root, "tokenizer.json", "tokenizer.json");
  ContainedRegularFile(canonical_root, "tokenizer_config.json", "tokenizer_config.json");
  result.config = GenAIConfig::LoadFromFile(config_path.string());
  result.execution_provider =
      NormalizeProvider(result.config.DefaultProvider());

  nlohmann::json manifest;
  try {
    std::ifstream stream(component_manifest);
    stream >> manifest;
  } catch (const nlohmann::json::exception& error) {
    throw std::invalid_argument(std::string("invalid component manifest: ") + error.what());
  }
  if (!manifest.is_object() || manifest.value("schema_version", 0) != 1 ||
      !manifest.contains("components") || !manifest["components"].is_object() ||
      manifest["components"].empty()) {
    throw std::invalid_argument(
        "component manifest requires schema_version 1 and non-empty components");
  }
  for (const auto& [name, component] : manifest["components"].items()) {
    if (name.empty() || !component.is_object()) {
      throw std::invalid_argument("component manifest contains an invalid component");
    }
    if (!component.contains("filename") ||
        !component["filename"].is_string() ||
        component["filename"].get_ref<const std::string&>().empty()) {
      throw std::invalid_argument(
          "component manifest component requires a non-empty filename");
    }
    const auto filename = component["filename"].get<std::string>();
    ContainedRegularFile(canonical_root, filename, "component filename");
  }
  result.estimated_resident_bytes = MeasureContainedPackage(canonical_root);
  return result;
}

}  // namespace fl
