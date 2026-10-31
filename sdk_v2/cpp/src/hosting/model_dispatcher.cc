// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/model_dispatcher.h"

#include "catalog.h"
#include "exception.h"
#include "items/item.h"
#include "items/text_item.h"
#include "util/path_safety.h"

#include <filesystem>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;

CatalogType Catalog(const Json& value) {
  const auto name = value.get<std::string>();
  if (name == "public") {
    return CatalogType::kPublic;
  }
  if (name == "local") {
    return CatalogType::kLocal;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK model catalog");
}

void RequireAbsolutePath(const std::string& path) {
  if (!path.empty() && !std::filesystem::path(path).is_absolute()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "SDK model artifact path must be absolute");
  }
}

bool SameArtifact(const std::string& left, const std::string& right) {
  return left.empty() || right.empty() ? left == right : ModelPathsEqual(left, right);
}

Json ItemDescriptors(const Item* const* items, size_t count) {
  Json result = Json::array();
  for (size_t i = 0; i < count; ++i) {
    Json descriptor{{"type", static_cast<int>(items[i]->type)}};
    if (items[i]->type == FOUNDRY_LOCAL_ITEM_TEXT) {
      descriptor["text_type"] = static_cast<const TextItem&>(*items[i]).text_type;
    }

    result.push_back(std::move(descriptor));
  }

  return result;
}

}  // namespace

ModelDispatcher::ModelDispatcher(std::shared_ptr<api::ManagerBackend> manager) : manager_(std::move(manager)) {
  if (!manager_ || manager_->Kind() != api::BackendKind::Embedded) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "host requires an embedded manager");
  }
}

bool ModelDispatcher::Handles(Operation operation) noexcept {
  switch (operation) {
    case Operation::RegisterModel:
    case Operation::UnregisterModel:
    case Operation::LoadedModels:
    case Operation::IsLoaded:
    case Operation::Load:
    case Operation::Unload:
    case Operation::ModelInputOutputInfo:
    case Operation::CreateSession:
    case Operation::RemoveFromCache:
      return true;
    default:
      return false;
  }
}

void ModelDispatcher::Dispatch(const std::shared_ptr<SdkConnection::IncomingCall>& call, HostObjects& objects) {
  SdkResult result;
  try {
    // Keep source lookup/capture consistent with mutations from other SDK connections, not with session execution.
    std::lock_guard lock(mutex_);
    if (call->IsCancelled()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "SDK invocation cancelled before execution");
    }

    result.value = Execute(*call, objects);
  } catch (const Exception& error) {
    result = {static_cast<int32_t>(error.code()), nullptr, error.what()};
  } catch (const Json::exception&) {
    result = {FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, nullptr, "invalid SDK model payload"};
  } catch (const std::exception& error) {
    result = {FOUNDRY_LOCAL_ERROR_INTERNAL, nullptr, error.what()};
  } catch (...) {
    result = {FOUNDRY_LOCAL_ERROR_INTERNAL, nullptr, "SDK model execution failed"};
  }

  call->Complete(std::move(result));
}

ModelDispatcher::ResolvedModel ModelDispatcher::Resolve(const ModelSelection& selection) {
  auto& catalog = manager_->GetCatalog(selection.catalog);
  auto* model = catalog.GetModelVariant(selection.info.model_id);
  if (!model) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "SDK model is no longer available in the selected catalog");
  }

  model->RefreshCachedState();
  auto captured = manager_->CaptureModel(*model);
  if (captured->Info() != selection.info || !SameArtifact(captured->GetPath(), selection.path)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "SDK model metadata or artifact changed; refresh the catalog selection");
  }

  return {*model, std::move(captured)};
}

Json ModelDispatcher::Execute(const SdkConnection::IncomingCall& call, HostObjects& objects) {
  const auto& payload = call.Payload();
  switch (call.Op()) {
    case Operation::RegisterModel: {
      auto& catalog = manager_->GetCatalog(Catalog(payload.at("catalog")));
      const auto path = payload.at("path").get<std::string>();
      RequireAbsolutePath(path);
      auto* model =
          catalog.RegisterModel(path, payload.at("id").get<std::string>(), DecodeModelInfo(payload.at("metadata")));
      const auto captured = manager_->CaptureModel(*model);
      return EncodeModelSelection(Catalog(payload.at("catalog")), *captured);
    }
    case Operation::UnregisterModel:
      manager_->GetCatalog(Catalog(payload.at("catalog")))
          .UnregisterModel(payload.at("alias_or_id").get<std::string>());
      return nullptr;
    case Operation::LoadedModels: {
      const auto type = Catalog(payload.at("catalog"));
      auto& catalog = manager_->GetCatalog(type);
      Json models = Json::array();
      for (auto* model : catalog.GetLoadedModels()) {
        const auto captured = manager_->CaptureModel(*model);
        models.push_back(EncodeModelSelection(type, *captured));
      }

      return models;
    }
    default:
      break;
  }

  const auto selection = DecodeModelSelection(payload.at("model"));
  if (selection.info.model_id.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "SDK model identity must not be empty");
  }

  RequireAbsolutePath(selection.path);
  if (call.Op() == Operation::IsLoaded) {
    return !selection.path.empty() && manager_->IsModelArtifactLoaded(selection.info.model_id, selection.path);
  }
  if (call.Op() == Operation::Unload) {
    // An uncached selection has no artifact; it must not target a model loaded under the same ID.
    if (!selection.path.empty()) {
      manager_->UnloadModelArtifact(selection.info.model_id, selection.path);
    }

    return nullptr;
  }
  if ((call.Op() == Operation::Load || call.Op() == Operation::CreateSession) && selection.path.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "SDK model has no selected local artifact; download the model first");
  }

  auto model = Resolve(selection);
  switch (call.Op()) {
    case Operation::Load:
      model.source.Load(selection.path);
      return nullptr;
    case Operation::RemoveFromCache:
      model.source.RemoveFromCache(selection.path);
      return nullptr;
    case Operation::ModelInputOutputInfo: {
      const auto info = model.execution->GetInputOutputInfo();
      return {{"inputs", ItemDescriptors(info.inputs, info.num_inputs)},
              {"outputs", ItemDescriptors(info.outputs, info.num_outputs)}};
    }
    case Operation::CreateSession: {
      return EncodeObjectIdentity(objects.AddSession(model.execution->CreateSession()));
    }
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "operation is not an SDK model operation");
  }
}

}  // namespace fl::hosting
