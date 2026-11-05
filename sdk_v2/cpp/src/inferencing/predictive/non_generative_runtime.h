// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "contracts/non_generative.h"

#include <nlohmann/json.hpp>
#include <ort_genai.h>

#include <memory>
#include <functional>
#include <stdexcept>
#include <string>

namespace fl {

class RuntimeCapacityError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class IRankingRuntime {
 public:
  virtual ~IRankingRuntime() = default;
  virtual nlohmann::json Rank(
      const RankingRequest& request,
      std::function<bool()> is_canceled = {}) = 0;
};

class IDecisionRuntime {
 public:
  virtual ~IDecisionRuntime() = default;
  virtual nlohmann::json Decide(
      const NonGenerativeRequest& request,
      std::function<bool()> is_canceled = {}) = 0;
};

class RankingRuntime final : public IRankingRuntime {
 public:
  explicit RankingRuntime(const std::string& package_path);
  ~RankingRuntime();
  nlohmann::json Rank(
      const RankingRequest& request,
      std::function<bool()> is_canceled = {}) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class DecisionRuntime final : public IDecisionRuntime {
 public:
  explicit DecisionRuntime(const std::string& package_path);
  ~DecisionRuntime();
  nlohmann::json Decide(
      const NonGenerativeRequest& request,
      std::function<bool()> is_canceled = {}) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fl
