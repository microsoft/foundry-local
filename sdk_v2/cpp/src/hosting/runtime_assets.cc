// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/runtime_assets.h"

#include "exception.h"
#include "hosting/sdk_connection.h"
#include "util/sha256.h"
#include "version.h"

#include <onnxruntime_c_api.h>
#include <ort_genai_c.h>

namespace fl::hosting {
namespace {

BootstrapArtifact Capture(const ResidentModule& module) {
  if (module.image_identity.empty() || FileModuleIdentity(module.path) != module.image_identity) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "resident runtime file was replaced since loading: ", module.path);
  }

  auto hash = Sha256File(module.path);
  if (hash.empty() || FileModuleIdentity(module.path) != module.image_identity) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "runtime file changed during asset verification: ", module.path);
  }

  return {module.path, std::move(hash)};
}

}  // namespace

RuntimeAssets ResolveRuntimeAssets(const ResidentModule& native, const ResidentModule& ort,
                                   const ResidentModule& genai) {
  RuntimeAssets assets;
  auto& bootstrap = assets.bootstrap;
  bootstrap.native = Capture(native);
  bootstrap.ort = Capture(ort);
  bootstrap.genai = Capture(genai);
  bootstrap.manifest = native.path.parent_path() / "foundry-local-runtime.json";
  bootstrap.native_build = FOUNDRY_LOCAL_NATIVE_BUILD_ID;
  bootstrap.runtime_identifier = FOUNDRY_LOCAL_RUNTIME_IDENTIFIER;
  const auto manifest = ReadBootstrapJson(bootstrap.manifest);
  const auto name = manifest.at("host").at("file_name").get<std::string>();
  const auto executable = std::filesystem::path(std::u8string(name.begin(), name.end()));
  if (name.find('\0') != std::string::npos || executable.empty() || executable.has_parent_path()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid native host filename in runtime manifest");
  }

  assets.executable = native.path.parent_path() / executable;
  VerifyBootstrapArtifacts(bootstrap, assets.executable, FOUNDRY_LOCAL_NATIVE_BUILD_ID,
                           FOUNDRY_LOCAL_RUNTIME_IDENTIFIER);
  assets.identity = {bootstrap.native_build,
                     bootstrap.runtime_identifier,
                     SdkConnection::kProtocolVersion};
  return assets;
}

RuntimeAssets ResolveRuntimeAssets() {
  static const char native_anchor = 0;
  return ResolveRuntimeAssets(ResidentModuleAt(&native_anchor), ResidentModuleAt(OrtGetApiBase()),
                              ResidentModuleAt(reinterpret_cast<const void*>(&OgaCreateModel)));
}

}  // namespace fl::hosting
