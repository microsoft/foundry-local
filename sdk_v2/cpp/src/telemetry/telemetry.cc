// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry.h"

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
      return "EpDownloadAttempt";
    case Action::kEpDownloadAndRegister:
      return "EpDownloadAndRegister";
    case Action::kModelFileDownload:
      return "ModelFileDownload";
    case Action::kModelInference:
      return "ModelInference";
    case Action::kServiceRequestUnmatched:
      return "ServiceRequestUnmatched";
    case Action::kServiceStatus:
      return "ServiceStatus";
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
    default:
      return "Unknown";
  }
}

}  // namespace fl
