// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"

#include <string>

namespace fl {

/// Result returned from a tool invocation.
struct ToolResultItem : Item {
  std::string call_id;
  std::string result;

  ToolResultItem(std::string call_id_in = {}, std::string result_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_TOOL_RESULT),
        call_id(std::move(call_id_in)),
        result(std::move(result_in)) {}

  void SetToolResultData(const flToolResultData& new_data) {
    call_id = new_data.call_id ? new_data.call_id : "";
    result = new_data.result ? new_data.result : "";
  }

  void GetApiData(flToolResultData& out) const {
    out.version = FOUNDRY_LOCAL_DATA_VERSION;
    out.call_id = call_id.c_str();
    out.result = result.c_str();
  }
};

}  // namespace fl
