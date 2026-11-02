// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/manager_dispatcher.h"

#include "ep_detection/ep_types.h"
#include "exception.h"

namespace fl::hosting {
namespace {

using Json = nlohmann::json;

}  // namespace

ManagerDispatcher::ManagerDispatcher(std::shared_ptr<api::ManagerBackend> manager, HostLifetime& lifetime)
    : manager_(std::move(manager)), lifetime_(lifetime) {
  if (!manager_ || manager_->Kind() != api::BackendKind::Embedded) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "host requires an embedded manager");
  }
}

bool ManagerDispatcher::Handles(Operation operation) noexcept {
  switch (operation) {
    case Operation::WebServiceStart:
    case Operation::WebServiceUrls:
    case Operation::WebServiceStop:
    case Operation::DiscoverableEps:
    case Operation::RuntimeDevices:
    case Operation::DownloadAndRegisterEps:
      return true;
    default:
      return false;
  }
}

void ManagerDispatcher::Dispatch(const std::shared_ptr<SdkConnection::IncomingCall>& call,
                                 std::optional<HostLifetime::OwnerId> owner) {
  SdkResult result;
  try {
    if (call->IsCancelled()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "SDK invocation cancelled before execution");
    }

    result.value = Execute(*call, owner);
  } catch (const Exception& error) {
    result = {static_cast<int32_t>(error.code()), nullptr, error.what()};
  } catch (const Json::exception&) {
    result = {FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, nullptr, "invalid SDK manager payload"};
  } catch (const std::exception& error) {
    result = {FOUNDRY_LOCAL_ERROR_INTERNAL, nullptr, error.what()};
  } catch (...) {
    result = {FOUNDRY_LOCAL_ERROR_INTERNAL, nullptr, "SDK manager execution failed"};
  }

  call->Complete(std::move(result));
}

Json ManagerDispatcher::Execute(SdkConnection::IncomingCall& call, std::optional<HostLifetime::OwnerId> owner) {
  switch (call.Op()) {
    case Operation::RuntimeDevices:
      return manager_->GetRuntimeDevices();
    case Operation::WebServiceStart: {
      if (!owner) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "web-service start requires a host owner");
      }

      std::lock_guard lock(web_mutex_);
      if (manager_->GetWebServiceUrls().empty()) {
        manager_->StartWebService();
      }

      return manager_->GetWebServiceUrls();
    }
    case Operation::WebServiceUrls: {
      std::lock_guard lock(web_mutex_);
      return manager_->GetWebServiceUrls();
    }
    case Operation::WebServiceStop: {
      std::lock_guard lock(web_mutex_);
      auto stop = owner ? lifetime_.TryAdmitWebServiceStop(*owner, std::chrono::steady_clock::now())
                        : HostLifetime::WorkGuard{};
      if (!stop) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "shared web service is busy with other owners or HTTP work");
      }

      manager_->StopWebService();
      return nullptr;
    }
    case Operation::DiscoverableEps: {
      Json result = Json::array();
      for (const auto& ep : manager_->GetDiscoverableEps()) {
        result.push_back({{"name", ep.name}, {"registered", ep.is_registered}});
      }

      return {{"eps", std::move(result)}, {"downloading", manager_->IsEpDownloadInProgress()}};
    }
    case Operation::DownloadAndRegisterEps: {
      if (!owner) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "provider registration requires a host owner");
      }

      const auto& names = call.Payload().at("names");
      const auto selected = names.is_null() ? std::optional<std::vector<std::string>>{}
                                            : std::optional{names.get<std::vector<std::string>>()};
      const auto result = manager_->DownloadAndRegisterEps(
          selected ? &*selected : nullptr, [&](const std::string& name, float percent) {
            if (call.IsCancelled()) {
              return false;
            }

            call.SendEvent({{"name", name}, {"percent", percent}});
            const auto response = call.ReadInput();
            return response && response->at("continue").get<bool>() && !call.IsCancelled();
          });
      return {{"success", result.success}, {"cancelled", result.cancelled}, {"status", result.status}, {"registered", result.registered_eps}, {"failed", result.failed_eps}, {"downloading", manager_->IsEpDownloadInProgress()}};
    }
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "operation is not an SDK manager operation");
  }
}

}  // namespace fl::hosting
