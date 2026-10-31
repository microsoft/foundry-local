// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <foundry_local/foundry_local_cpp.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef GetMessage
#undef GetMessage
#endif

namespace sample {

inline constexpr std::string_view kAppName = "foundry_local_samples";

inline void RegisterExecutionProviders(foundry_local::Manager& manager) {
  manager.DownloadAndRegisterEps({}, [](std::string_view name, float percent) {
    std::cout << "\rEP " << name << ": " << static_cast<int>(percent) << "%" << std::flush;
    return true;
  });
  std::cout << "\n";
}

inline std::unique_ptr<foundry_local::IModel> GetModel(foundry_local::ICatalog& catalog,
                                                       const std::string& alias) {
  auto model = catalog.GetModel(alias);
  if (!model) {
    throw std::runtime_error("Model not found in catalog: " + alias);
  }

  return model;
}

inline void DownloadAndLoad(foundry_local::IModel& model) {
  if (!model.IsCached()) {
    model.Download([](float percent) {
      std::cout << "\rModel download: " << static_cast<int>(percent) << "%" << std::flush;
      return 0;
    });
    std::cout << "\n";
  } else {
    std::cout << "Using cached model at " << model.GetPath() << "\n";
  }

  if (!model.IsLoaded()) {
    model.Load();
  }
}

class LoadedModel final {
 public:
  explicit LoadedModel(foundry_local::IModel& model) : model_(model) {
    DownloadAndLoad(model_);
  }

  ~LoadedModel() {
    try {
      if (model_.IsLoaded()) {
        model_.Unload();
      }
    } catch (...) {
    }
  }

  LoadedModel(const LoadedModel&) = delete;
  LoadedModel& operator=(const LoadedModel&) = delete;

 private:
  foundry_local::IModel& model_;
};

inline void PrintAssistantMessages(const foundry_local::Response& response) {
  for (const auto& item : response.GetItems()) {
    if (item.GetType() != FOUNDRY_LOCAL_ITEM_MESSAGE) {
      continue;
    }

    const auto message = item.GetMessage();
    for (const auto& part : message.parts) {
      if (part.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
        const auto text = part.GetText();
        if (text.type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
          std::cout << text.text;
        }
      }
    }
  }
  std::cout << "\n";
}

}  // namespace sample
