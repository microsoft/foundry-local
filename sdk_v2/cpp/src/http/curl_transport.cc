// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "http/curl_transport.h"

#if !defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)

#include "http/http_client.h"

#include <string>
#include <utility>

namespace fl {
namespace http {

const Azure::Core::Http::CurlTransportOptions& CachedCurlTransportOptions() {
  static const Azure::Core::Http::CurlTransportOptions options = [] {
    Azure::Core::Http::CurlTransportOptions opts;
    if (std::string ca_bundle = CaBundleFile(); !ca_bundle.empty()) {
      opts.CAInfo = std::move(ca_bundle);
    }
    return opts;
  }();
  return options;
}

}  // namespace http
}  // namespace fl

#endif  // !FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT
