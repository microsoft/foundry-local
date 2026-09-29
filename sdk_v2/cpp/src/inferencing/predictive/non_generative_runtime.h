// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "contracts/non_generative.h"

#include <nlohmann/json.hpp>
#include <ort_genai.h>

#include <memory>
#include <string>

namespace fl {

class RankingRuntime {
 public:
  explicit RankingRuntime(const std::string& package_path);
  RankingRuntime(const std::string& package_path, std::string provider);
  ~RankingRuntime();
  nlohmann::json Rank(const NonGenerativeRequest& request);
  nlohmann::json Rank(const RankingRequest& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class DecisionRuntime {
 public:
  explicit DecisionRuntime(const std::string& package_path);
  DecisionRuntime(const std::string& package_path, std::string provider);
  ~DecisionRuntime();
  nlohmann::json Decide(const NonGenerativeRequest& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fl
