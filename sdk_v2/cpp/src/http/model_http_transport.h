// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "http/http_client.h"

#include <foundry_local/foundry_local_c.h>

#include <memory>
#include <string>

namespace Azure::Core::Http {
class HttpTransport;
}

namespace fl::http {

/// Consume a non-null status inside its owning SDK, without an interposable exported API lookup.
flErrorCode ConsumeModelHttpStatus(flStatus& status) noexcept;

/// One retained callback context, shared by configuration snapshots, clients, and outstanding operations.
class ModelHttpTransport final : public std::enable_shared_from_this<ModelHttpTransport> {
 public:
  explicit ModelHttpTransport(const flHttpTransport& callbacks);
  ~ModelHttpTransport();
  ModelHttpTransport(const ModelHttpTransport&) = delete;
  ModelHttpTransport& operator=(const ModelHttpTransport&) = delete;

  const flHttpTransport& Callbacks() const noexcept { return callbacks_; }

  /// Metadata responses are bounded to 16 MiB. Blob responses never use this buffered path.
  HttpResponse Request(const std::string& method, const std::string& url, const std::string& body = {},
                       const HttpRequestOptions& options = {});
  std::shared_ptr<Azure::Core::Http::HttpTransport> AzureTransport();

 private:
  flHttpTransport callbacks_;
};

}  // namespace fl::http
