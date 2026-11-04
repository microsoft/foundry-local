// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <vector>
#include <string>
#include <unordered_set>
#include <utility>

namespace fl {

/// One hop of a reconstructed Responses chain: the input items that request was given, followed by the output items
/// it produced.
///
/// The two are kept apart because a hop's output is exactly one assistant turn. Flattening them into a single array
/// loses that boundary: `message -> function_call -> message` would rebuild as two assistant messages instead of the
/// one the live session committed, and a hop whose output was reasoning-only or empty would leave no assistant turn
/// at all, putting two user turns next to each other in the rebuilt conversation.
///
/// This is an internal replay representation. It never reaches the wire — the Responses response shape and the
/// `/input_items` endpoint are unchanged.
struct ResponseChainHop {
  ResponseChainHop() = default;
  ResponseChainHop(nlohmann::json input, nlohmann::json output,
                   std::unordered_set<std::string> raw_call_ids = {})
      : input_items(std::move(input)),
        output_items(std::move(output)),
        raw_envelope_call_ids(std::move(raw_call_ids)) {}

  nlohmann::json input_items = nlohmann::json::array();
  nlohmann::json output_items = nlohmann::json::array();
  std::unordered_set<std::string> raw_envelope_call_ids;
};

/// A complete chain, oldest hop first.
using ResponseChainContext = std::vector<ResponseChainHop>;

}  // namespace fl
