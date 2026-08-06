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

/// Returns the process-wide libcurl transport options, with `CAInfo` populated from `SSL_CERT_FILE`
/// (via `CaBundleFile`). Built once and shared by every libcurl transport we construct — direct
/// requests, file downloads, and the Azure Storage blob client — because the CA bundle path is fixed
/// for the process lifetime. When `SSL_CERT_FILE` is unset, `CAInfo` is empty and libcurl falls back
/// to its compiled-in default. The returned reference is valid for the lifetime of the process.
const Azure::Core::Http::CurlTransportOptions& CachedCurlTransportOptions();

}  // namespace http
}  // namespace fl

#endif  // !FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT
