// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <memory>

namespace oatpp::web::server {
class HttpRequestHandler;
}

namespace fl {
struct ServiceContext;

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx);

}  // namespace fl
