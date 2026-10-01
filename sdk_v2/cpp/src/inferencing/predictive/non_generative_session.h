// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/predictive/non_generative_runtime.h"
#include "inferencing/session/session.h"

#include <memory>
#include <string>

namespace fl {

class NonGenerativeSession final : public Session {
 public:
  NonGenerativeSession(const Model& model, ILogger& logger, ITelemetry& telemetry);
  ~NonGenerativeSession() override;

  SessionType Type() const override { return SessionType::kPredictive; }

 protected:
  void ProcessRequestImpl(const Request& request, Response& response) override;
  std::string ExecutionProvider() const override { return provider_; }

 private:
  Model* model_{};
  std::string model_id_;
  std::string provider_;
  std::unique_ptr<RankingRuntime> ranking_;
  std::unique_ptr<DecisionRuntime> decision_;
};

}  // namespace fl
