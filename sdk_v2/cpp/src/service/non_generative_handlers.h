// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <memory>

namespace oatpp::web::server {
class HttpRequestHandler;
}

namespace fl {
class Model;
class NonGenerativeRuntimeState;
struct ServiceContext;

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState();
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx);
bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model);
void ClearNonGenerativeRuntimes(ServiceContext& ctx);

}  // namespace fl
