// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "http/curl_transport.h"

#if !defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)

#include "http/http_client.h"

#include <string>

namespace fl {
namespace http {

Azure::Core::Http::CurlTransportOptions MakeCurlTransportOptions() {
  Azure::Core::Http::CurlTransportOptions options;
  if (const std::string& ca_bundle = CABundleFilePath(); !ca_bundle.empty()) {
    options.CAInfo = ca_bundle;
  }

  return options;
}

}  // namespace http
}  // namespace fl

#endif  // !FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT
