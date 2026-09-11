// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"

#include <string>

namespace fl {

/// Model-generated request to invoke a tool.
struct ToolCallItem : Item {
  std::string call_id;
  std::string name;
  std::string arguments;
  // True only for a call reconstructed from an already-committed response-store item. Such calls
  // use generated-call normalization on replay so a malformed function call remains replayable,
  // while a custom call keeps the raw text that its later kind snapshot identifies.
  bool replayed_from_store = false;
  std::string replayed_arguments;

  ToolCallItem(std::string call_id_in = {}, std::string name_in = {}, std::string arguments_in = {},
               bool replayed_from_store_in = false)
      : Item(FOUNDRY_LOCAL_ITEM_TOOL_CALL),
        call_id(std::move(call_id_in)),
        name(std::move(name_in)),
        arguments(std::move(arguments_in)),
        replayed_from_store(replayed_from_store_in) {}

  void SetToolCallData(const flToolCallData& new_data) {
    call_id = new_data.call_id ? new_data.call_id : "";
    name = new_data.name ? new_data.name : "";
    arguments = new_data.arguments ? new_data.arguments : "";
    replayed_from_store = false;
    replayed_arguments.clear();
  }

  void GetApiData(flToolCallData& out) const {
    out.version = FOUNDRY_LOCAL_API_VERSION;
    out.call_id = call_id.c_str();
    out.name = name.c_str();
    out.arguments = arguments.c_str();
  }
};

}  // namespace fl
