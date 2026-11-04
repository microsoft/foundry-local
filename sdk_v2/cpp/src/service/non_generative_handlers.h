// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/predictive/non_generative_admission.h"

#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace oatpp::web::server {
class HttpRequestHandler;
}

namespace fl {
class Model;
struct ServiceContext;

std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx);
bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model);
void ClearNonGenerativeRuntimes(ServiceContext& ctx);

}  // namespace fl
#include <cstdint>
