// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "hosting/host_objects.h"
#include "hosting/sdk_connection.h"
#include "hosting/sdk_values.h"

#include <mutex>

namespace fl::hosting {

// Shared by the host's connections. Resolves live catalog selections, never caller-supplied execution metadata.
class ModelDispatcher {
 public:
  explicit ModelDispatcher(std::shared_ptr<api::ManagerBackend> manager);
  static bool Handles(Operation operation) noexcept;
  void Dispatch(const std::shared_ptr<SdkConnection::IncomingCall>& call, HostObjects& objects);

 private:
  struct ResolvedModel {
    api::ModelBackend& source;
    std::unique_ptr<api::ModelBackend> execution;
  };
  ResolvedModel Resolve(const ModelSelection& selection);
  nlohmann::json Execute(const SdkConnection::IncomingCall& call, HostObjects& objects);

  const std::shared_ptr<api::ManagerBackend> manager_;
  std::mutex mutex_;
};

}  // namespace fl::hosting
