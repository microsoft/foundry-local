// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/child_host_role.h"

#include "exception.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace fl::hosting {
namespace {

void Validate(const ChildHostRole& role) {
  if (role.endpoint.empty() || role.profile_key.size() != 64 || role.incarnation.empty() ||
      role.incarnation.size() > 128 || role.starter.pid == 0 || role.starter.generation == 0 ||
      !std::all_of(role.profile_key.begin(), role.profile_key.end(),
                   [](char value) { return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F'); })) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid native child-host bootstrap descriptor");
  }
}

}  // namespace

std::string EncodeChildHostRole(const ChildHostRole& role) {
  Validate(role);
  return nlohmann::json{{"version", 1},
                        {"endpoint", role.endpoint.string()},
                        {"profile", role.profile_key},
                        {"incarnation", role.incarnation},
                        {"starter", {{"pid", role.starter.pid}, {"generation", role.starter.generation}}}}
      .dump();
}

ChildHostRole DecodeChildHostRole(std::string_view value) {
  try {
    const auto json = nlohmann::json::parse(value);
    if (json.at("version") != 1 || !json.at("starter").at("pid").is_number_unsigned() ||
        !json.at("starter").at("generation").is_number_unsigned()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported native child-host bootstrap descriptor");
    }

    ChildHostRole role{
        json.at("endpoint").get<std::string>(),
        json.at("profile").get<std::string>(),
        json.at("incarnation").get<std::string>(),
        {json.at("starter").at("pid").get<uint64_t>(), json.at("starter").at("generation").get<uint64_t>()}};
    Validate(role);
    return role;
  } catch (const nlohmann::json::exception&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid native child-host bootstrap descriptor");
  }
}

}  // namespace fl::hosting
