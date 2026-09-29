// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace fl {

struct NonGenerativeQuestion {
  std::string type;
  nlohmann::ordered_json criteria;
  nlohmann::ordered_json instructions;
};

struct NonGenerativeRequest {
  nlohmann::ordered_json state;
  std::vector<std::pair<std::string, NonGenerativeQuestion>> questions;
  std::string model{"kev"};
  float temperature = 1.0f;
};

struct RankingRequest {
  nlohmann::ordered_json context;
  std::string question;
  std::vector<std::string> answers;
  std::string model{"clm"};
  float temperature{1.0f};
};

void from_json(const nlohmann::ordered_json& json, NonGenerativeQuestion& question);
void from_json(const nlohmann::ordered_json& json, NonGenerativeRequest& request);
void from_json(const nlohmann::ordered_json& json, RankingRequest& request);

}  // namespace fl
