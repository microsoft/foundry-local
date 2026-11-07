// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/model_load_manager.h"
#include "inferencing/predictive/non_generative_runtime.h"
#include "inferencing/session/session.h"

#include <memory>
#include <optional>
#include <string>

namespace fl {

class NonGenerativeSession final : public Session {
 public:
  NonGenerativeSession(const Model& model, ModelLoadManager& model_load_manager,
                       ILogger& logger, ITelemetry& telemetry);
  ~NonGenerativeSession() override;

  SessionType Type() const override { return SessionType::kPredictive; }

 protected:
  void ProcessRequestImpl(const Request& request, Response& response) override;
  std::shared_ptr<void> AcquireRequestLifetime() override;
  std::string ExecutionProvider() const override { return provider_; }

 private:
  struct Lifecycle {
    std::mutex mutex;
    NonGenerativeSession* session{};
    ModelLoadManager* manager{};
  };
  void ReleaseRuntime();
  std::optional<ModelLoadManager::PredictiveRequestLease>
      construction_lifetime_;
  std::unique_ptr<Model> execution_model_;
  std::shared_ptr<Lifecycle> lifecycle_;
  std::string model_id_;
  std::string model_alias_;
  std::string provider_;
  std::shared_ptr<IRankingRuntime> ranking_;
  std::shared_ptr<IDecisionRuntime> decision_;
};

}  // namespace fl
