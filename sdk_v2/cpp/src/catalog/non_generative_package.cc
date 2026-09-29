// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/non_generative_package.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace fl {
namespace {

namespace fs = std::filesystem;

std::string RequiredString(const nlohmann::json& object, const char* key) {
  if (!object.contains(key) || !object[key].is_string() ||
      object[key].get_ref<const std::string&>().empty()) {
    throw std::invalid_argument(std::string("inference_model.json requires non-empty ") + key);
  }
  return object[key].get<std::string>();
}

fs::path SafeRelativePath(const std::string& relative, const char* field) {
  const fs::path path(relative);
  if (path.empty() || path.is_absolute()) {
    throw std::invalid_argument(std::string(field) + " must be package-relative");
  }
  for (const auto& part : path) {
    if (part == "..") {
      throw std::invalid_argument(std::string(field) + " must not contain '..'");
    }
  }
  return path;
}

bool IsWithin(const fs::path& root, const fs::path& candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end() && candidate_it != candidate.end();
       ++root_it, ++candidate_it) {
    if (*root_it != *candidate_it) return false;
  }
  return root_it == root.end();
}

fs::path ContainedRegularFile(const fs::path& canonical_root,
                              const std::string& relative, const char* field) {
  const auto candidate = canonical_root / SafeRelativePath(relative, field);
  std::error_code ec;
  const auto canonical_candidate = fs::canonical(candidate, ec);
  if (ec || !fs::is_regular_file(canonical_candidate, ec)) {
    throw std::invalid_argument(std::string(field) + " does not name an existing file");
  }
  if (!IsWithin(canonical_root, canonical_candidate)) {
    throw std::invalid_argument(std::string(field) + " resolves outside the package root");
  }
  return canonical_candidate;
}

}  // namespace

std::string NormalizeNonGenerativeProvider(std::string_view provider) {
  std::string name(provider);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  constexpr std::string_view suffix = "executionprovider";
  if (name.size() > suffix.size() && name.ends_with(suffix)) {
    name.resize(name.size() - suffix.size());
  }
  if (name == "cpu") return {};
  if (name == "cuda") return "cuda";
  if (name == "qnn") return "QNN";
  if (name == "webgpu") return "WebGPU";
  if (name == "dml" || name == "directml") return "DML";
  if (name == "openvino") return "OpenVINO";
  if (name == "vitisai") return "VitisAI";
  if (name == "ryzenai") return "RyzenAI";
  if (name == "nvtensorrtrtx") return "NvTensorRtRtx";
  if (name == "amdgpu") return "AMDGPU";
  throw std::invalid_argument("unsupported execution provider: " + std::string(provider));
}

std::optional<NonGenerativePackageMetadata> ReadNonGenerativePackage(
    const fs::path& package_root) {
  std::error_code ec;
  const auto canonical_root = fs::canonical(package_root, ec);
  if (ec || !fs::is_directory(canonical_root, ec)) return std::nullopt;
  if (!fs::exists(canonical_root / "inference_model.json")) return std::nullopt;
  const auto inference_path =
      ContainedRegularFile(canonical_root, "inference_model.json", "inference_model.json");

  nlohmann::json inference;
  try {
    std::ifstream stream(inference_path);
    if (!stream) throw std::invalid_argument("cannot open inference_model.json");
    stream >> inference;
  } catch (const nlohmann::json::exception& error) {
    throw std::invalid_argument(std::string("invalid inference_model.json: ") + error.what());
  }
  if (!inference.is_object() || !inference.contains("ComponentManifest")) {
    return std::nullopt;
  }
  if (fs::exists(canonical_root / "download.tmp")) {
    throw std::invalid_argument("package contains download.tmp");
  }

  NonGenerativePackageMetadata result;
  result.model_id = RequiredString(inference, "Name");
  result.alias = RequiredString(inference, "Alias");
  result.task = RequiredString(inference, "Task");
  if (result.task != "text-ranking" && result.task != "typed-decision") {
    throw std::invalid_argument("Task must be text-ranking or typed-decision");
  }
  RequiredString(inference, "License");

  if (!inference.contains("Provenance") || !inference["Provenance"].is_object()) {
    throw std::invalid_argument("inference_model.json requires Provenance");
  }
  const auto& provenance = inference["Provenance"];
  RequiredString(provenance, "source");
  RequiredString(provenance, "artifact_revision");
  RequiredString(provenance, "base_model");
  RequiredString(provenance, "base_revision");

  if (!inference.contains("Provider") || !inference["Provider"].is_object()) {
    throw std::invalid_argument("inference_model.json requires Provider");
  }
  result.execution_provider = NormalizeNonGenerativeProvider(
      RequiredString(inference["Provider"], "execution_provider"));
  result.provider_variant = RequiredString(inference["Provider"], "variant");

  if (!inference.contains("Capabilities") || !inference["Capabilities"].is_array() ||
      inference["Capabilities"].empty()) {
    throw std::invalid_argument("inference_model.json requires non-empty Capabilities");
  }
  std::unordered_set<std::string> unique_capabilities;
  for (const auto& capability : inference["Capabilities"]) {
    if (!capability.is_string() || capability.get_ref<const std::string&>().empty()) {
      throw std::invalid_argument("Capabilities entries must be non-empty strings");
    }
    auto value = capability.get<std::string>();
    if (!unique_capabilities.insert(value).second) {
      throw std::invalid_argument("Capabilities must not contain duplicates");
    }
    result.capabilities.push_back(std::move(value));
  }

  const auto manifest_relative = RequiredString(inference, "ComponentManifest");
  if (manifest_relative != "component_manifest.json") {
    throw std::invalid_argument(
        "ComponentManifest must be exactly component_manifest.json");
  }
  result.component_manifest =
      ContainedRegularFile(canonical_root, manifest_relative, "ComponentManifest");
  ContainedRegularFile(canonical_root, "tokenizer.json", "tokenizer.json");
  ContainedRegularFile(canonical_root, "tokenizer_config.json", "tokenizer_config.json");

  nlohmann::json manifest;
  try {
    std::ifstream stream(result.component_manifest);
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
  std::unordered_set<std::string> component_names;
  for (const auto& [name, component] : manifest["components"].items()) {
    if (name.empty() || !component.is_object()) {
      throw std::invalid_argument("component manifest contains an invalid component");
    }
    component_names.insert(name);
    const auto filename = RequiredString(component, "filename");
    ContainedRegularFile(canonical_root, filename, "component filename");
  }
  const auto has = [&](std::string_view name) {
    return component_names.contains(std::string(name));
  };
  if (result.task == "text-ranking") {
    const bool has_backbone = has("encoder") || has("backbone");
    const bool has_heads = has("clm_heads") ||
                           (has("state_head") && has("action_head") && has("scorer"));
    if (!has_backbone || !has_heads) {
      throw std::invalid_argument(
          "text-ranking requires encoder/backbone and clm_heads or "
          "state_head/action_head/scorer");
    }
  } else if (!has("backbone") || (!has("pointer_head") && !has("kev_head"))) {
    throw std::invalid_argument(
        "typed-decision requires backbone and pointer_head/kev_head");
  }
  return result;
}

}  // namespace fl
