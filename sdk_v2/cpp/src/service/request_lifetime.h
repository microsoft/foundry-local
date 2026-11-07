// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <memory>
#include <utility>

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include <oatpp/web/server/HttpRequestHandler.hpp>
#endif

namespace fl {
namespace hosting {
class HostLifetime;
}

// Propagates an HTTP admission to workers started while its handler executes. No work token means embedded mode.
class HttpRequestLifetime {
 public:
  explicit HttpRequestLifetime(std::shared_ptr<void> work) noexcept
      : previous_(std::exchange(current_, std::move(work))) {}
  ~HttpRequestLifetime() { current_ = std::move(previous_); }
  HttpRequestLifetime(const HttpRequestLifetime&) = delete;
  HttpRequestLifetime& operator=(const HttpRequestLifetime&) = delete;

  static std::shared_ptr<void> CurrentWork() noexcept { return current_; }

 private:
  inline static thread_local std::shared_ptr<void> current_;
  std::shared_ptr<void> previous_;
};

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
std::shared_ptr<oatpp::web::server::HttpRequestHandler> WithSharedHostAdmission(
    std::shared_ptr<oatpp::web::server::HttpRequestHandler> handler, std::shared_ptr<hosting::HostLifetime> lifetime);
#endif

}  // namespace fl
