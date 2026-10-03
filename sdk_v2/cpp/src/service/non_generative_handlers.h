// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace oatpp::web::server {
class HttpRequestHandler;
}

namespace fl {
class Model;
struct ServiceContext;

class NonGenerativeRuntimeState {
 public:
  std::shared_ptr<void> Acquire(
      const std::string& identity, const std::string& model_id, Model* owner,
      const std::function<std::shared_ptr<void>()>& factory);

 private:
  struct RuntimeEntry {
    std::string model_id;
    Model* owner;
    std::shared_future<std::shared_ptr<void>> runtime;
    std::shared_ptr<void> reservation;
  };

  std::mutex mutex_;
  std::map<std::string, RuntimeEntry> runtimes_;

  friend bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model);
  friend void ClearNonGenerativeRuntimes(ServiceContext& ctx);
};

std::shared_ptr<NonGenerativeRuntimeState> CreateNonGenerativeRuntimeState();
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateSystemOneHandler(ServiceContext& ctx);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> CreateRankHandler(ServiceContext& ctx);
bool UnloadNonGenerativeRuntime(ServiceContext& ctx, Model& model);
void ClearNonGenerativeRuntimes(ServiceContext& ctx);

}  // namespace fl
