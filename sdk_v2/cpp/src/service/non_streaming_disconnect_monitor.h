// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "inferencing/session/request.h"

#include <oatpp/network/tcp/Connection.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <cerrno>
#include <sys/socket.h>
#endif

namespace fl {

// No response is written until non-streaming inference finishes, so oatpp cannot notice a closed peer on its own.
class NonStreamingDisconnectMonitor {
 public:
  NonStreamingDisconnectMonitor(std::shared_ptr<oatpp::data::stream::IOStream> connection, Request& request)
      : tcp_(std::dynamic_pointer_cast<oatpp::network::tcp::Connection>(std::move(connection))), request_(request) {
    if (!tcp_) {
      throw std::invalid_argument("Non-streaming disconnect monitoring requires a TCP connection");
    }

    thread_ = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        if (PeerDisconnected(tcp_->getHandle())) {
          request_.CancelCurrentOrNext();
          disconnected_.store(true);
          return;
        }

        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(10), [&] { return stop.stop_requested(); });
      }
    });
  }

  NonStreamingDisconnectMonitor(const NonStreamingDisconnectMonitor&) = delete;
  NonStreamingDisconnectMonitor& operator=(const NonStreamingDisconnectMonitor&) = delete;

  ~NonStreamingDisconnectMonitor() { Stop(); }

  bool Stop() {
    if (thread_.joinable()) {
      thread_.request_stop();
      cv_.notify_one();
      thread_.join();
      if (!disconnected_.load() && PeerDisconnected(tcp_->getHandle())) {
        request_.CancelCurrentOrNext();
        disconnected_.store(true);
      }
    }

    return disconnected_.load();
  }

 private:
  static bool PeerDisconnected(oatpp::v_io_handle handle) {
    // A FIN only closes the peer's sending direction; HTTP clients may still be waiting for the response.
#ifdef _WIN32
    // Winsock reports a reset through recv even when SO_ERROR is zero. Readability makes the peek nonblocking.
    const auto socket = static_cast<SOCKET>(handle);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout{};
    const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
    if (ready == SOCKET_ERROR) {
      return ::WSAGetLastError() != WSAEINTR;
    }
    if (ready == 0) {
      return false;
    }

    char byte;
    if (::recv(socket, &byte, 1, MSG_PEEK) == SOCKET_ERROR) {
      const int error = ::WSAGetLastError();
      return error != WSAEWOULDBLOCK && error != WSAEINTR;
    }
    return false;
#else
    // SO_ERROR detects resets behind unread input without consuming request bytes.
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(static_cast<int>(handle), SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
      return errno != EINTR;
    }
    return error != 0;
#endif
  }

  std::shared_ptr<oatpp::network::tcp::Connection> tcp_;
  Request& request_;
  std::atomic_bool disconnected_{false};
  std::mutex mutex_;
  std::condition_variable cv_;
  std::jthread thread_;
};

}  // namespace fl

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
