// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry.h"

#include "exception.h"

namespace fl {

std::string_view ActionToString(Action action) {
  switch (action) {
    case Action::kInvalid:
      return "Invalid";
    case Action::kCoreInitialize:
      return "CoreInitialize";
    case Action::kCoreServiceStart:
      return "CoreServiceStart";
    case Action::kCoreServiceStop:
      return "CoreServiceStop";
    case Action::kSessionCreate:
      return "SessionCreate";
    case Action::kSessionProcessRequest:
      return "SessionProcessRequest";
    case Action::kModelLoad:
      return "ModelLoad";
    case Action::kModelUnload:
      return "ModelUnload";
    case Action::kModelList:
      return "ModelList";
    case Action::kOpenAIChatCompletions:
      return "OpenAIChatCompletions";
    case Action::kOpenAIModelList:
      return "OpenAIModelList";
    case Action::kOpenAIModelRetrieve:
      return "OpenAIModelRetrieve";
    case Action::kOpenAIAudioTranscribe:
      return "OpenAIAudioTranscribe";
    case Action::kOpenAIEmbeddings:
      return "OpenAIEmbeddings";
    case Action::kOpenAIResponsesCreate:
      return "OpenAIResponsesCreate";
    case Action::kOpenAIResponsesGet:
      return "OpenAIResponsesGet";
    case Action::kOpenAIResponsesList:
      return "OpenAIResponsesList";
    case Action::kOpenAIResponsesDelete:
      return "OpenAIResponsesDelete";
    case Action::kOpenAIResponsesGetInputItems:
      return "OpenAIResponsesGetInputItems";
    case Action::kEpDownloadAttempt:
      return "EPDownloadAttempt";
    case Action::kEpDownloadAndRegister:
      return "EPDownloadAndRegister";
    case Action::kModelFileDownload:
      return "ModelFileDownload";
    case Action::kModelInference:
      return "ModelInference";
    default:
      return "Unknown";
  }
}

std::string_view ActionStatusToString(ActionStatus status) {
  switch (status) {
    case ActionStatus::kFailure:
      return "Failure";
    case ActionStatus::kSuccess:
      return "Success";
    case ActionStatus::kInvalid:
      return "Invalid";
    case ActionStatus::kSkipped:
      return "Skipped";
    case ActionStatus::kClientError:
      return "ClientError";
    case ActionStatus::kCanceled:
      return "Canceled";
    case ActionStatus::kDependencyFailure:
      return "DependencyFailure";
    case ActionStatus::kTimeout:
      return "Timeout";
    default:
      return "Unknown";
  }
}

ActionStatus ActionStatusFromException(const std::exception& exception) {
  const auto* foundry_exception = dynamic_cast<const Exception*>(&exception);
  if (foundry_exception == nullptr) {
    return ActionStatus::kFailure;
  }

  switch (foundry_exception->code()) {
    case FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT:
    case FOUNDRY_LOCAL_ERROR_INVALID_USAGE:
      return ActionStatus::kClientError;
    case FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED:
      return ActionStatus::kCanceled;
    case FOUNDRY_LOCAL_ERROR_NETWORK:
      return ActionStatus::kDependencyFailure;
    default:
      return ActionStatus::kFailure;
  }
}

}  // namespace fl
