// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "hosting/host_objects.h"
#include "hosting/sdk_connection.h"

namespace fl::hosting {

// Called only by admitted execution workers, never by the protocol reader.
class SessionDispatcher {
 public:
  SessionDispatcher(HostObjects& objects, SdkConnection& connection) : objects_(objects), connection_(connection) {}
  static bool Handles(Operation operation) noexcept;
  void Dispatch(const std::shared_ptr<SdkConnection::IncomingCall>& call);

 private:
  void Process(const std::shared_ptr<SdkConnection::IncomingCall>& call,
               const std::shared_ptr<HostObjects::SessionResource>& session);

  HostObjects& objects_;
  SdkConnection& connection_;
};

}  // namespace fl::hosting
