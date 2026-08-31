// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

// Desktop Windows uses the WinHTTP transport and never links libcurl, so the curl transport options
// are only meaningful for the non-WinHTTP (libcurl) builds. Guard the whole header accordingly so
// includers on Windows do not pull in the Azure curl transport dependency.
#if !defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)

#include <azure/core/http/curl_transport.hpp>

namespace fl {
namespace http {

/// Creates libcurl transport options with `CAInfo` set from `CABundleFilePath` (Android only; empty
/// elsewhere, so libcurl falls back to the system default CA store).
Azure::Core::Http::CurlTransportOptions MakeCurlTransportOptions();

}  // namespace http
}  // namespace fl

#endif  // !FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT
