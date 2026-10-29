// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"
#include "items/item_queue.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace fl::hosting {

struct ItemEncodingContext {
  std::filesystem::path caller_working_directory{};
  std::function<uint64_t(const ItemQueue&)> register_queue{};
};

struct ItemDecodingContext {
  std::function<std::unique_ptr<ItemQueue>(uint64_t)> open_queue;
};

// Invoke at processing admission, not when the caller adds an item to a request.
nlohmann::json EncodeItem(const Item& item, const ItemEncodingContext& context);
std::unique_ptr<Item> DecodeItem(const nlohmann::json& value, const ItemDecodingContext& context);

}  // namespace fl::hosting
