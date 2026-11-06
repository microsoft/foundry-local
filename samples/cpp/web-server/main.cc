// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_cpp.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "samples/cpp/sample_common.h"

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

class SocketHandle final {
 public:
  explicit SocketHandle(Socket socket) : socket_(socket) {}
  ~SocketHandle() {
#ifdef _WIN32
    closesocket(socket_);
#else
    close(socket_);
#endif
  }

  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;
  Socket get() const noexcept { return socket_; }

 private:
  Socket socket_;
};

#ifdef _WIN32
class Winsock final {
 public:
  Winsock() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
  }

  ~Winsock() {
    WSACleanup();
  }
};
#endif

struct Endpoint {
  std::string host;
  std::string port;
};

Endpoint ParseEndpoint(std::string_view url) {
  constexpr std::string_view prefix = "http://";
  if (!url.starts_with(prefix)) {
    throw std::runtime_error("The sample HTTP client supports only http:// endpoints");
  }

  url.remove_prefix(prefix.size());
  const auto separator = url.rfind(':');
  if (separator == std::string_view::npos) {
    throw std::runtime_error("Web-service endpoint has no port");
  }

  return {std::string(url.substr(0, separator)), std::string(url.substr(separator + 1))};
}

std::string PostChatCompletion(const std::string& endpoint_url, const std::string& model_id) {
#ifdef _WIN32
  Winsock winsock;
#endif
  const auto endpoint = ParseEndpoint(endpoint_url);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  if (getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &addresses) != 0) {
    throw std::runtime_error("Could not resolve the web-service endpoint");
  }

  Socket socket = kInvalidSocket;
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket != kInvalidSocket && connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
      break;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
    socket = kInvalidSocket;
  }
  freeaddrinfo(addresses);
  if (socket == kInvalidSocket) {
    throw std::runtime_error("Could not connect to the web service");
  }
  SocketHandle connection(socket);

  const std::string body =
      R"({"model":")" + model_id +
      R"(","messages":[{"role":"user","content":"Why is the sky blue?."}],"stream":false})";
  const std::string request =
      "POST /v1/chat/completions HTTP/1.1\r\nHost: " + endpoint.host +
      "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  size_t sent = 0;
  while (sent < request.size()) {
    const auto count = send(connection.get(), request.data() + sent,
                            static_cast<int>(request.size() - sent), 0);
    if (count <= 0) {
      throw std::runtime_error("Failed to send the OpenAI-compatible request");
    }
    sent += static_cast<size_t>(count);
  }

  std::string response;
  std::array<char, 4096> buffer{};
  while (const auto count = recv(connection.get(), buffer.data(), static_cast<int>(buffer.size()), 0)) {
    if (count < 0) {
      throw std::runtime_error("Failed to read the OpenAI-compatible response");
    }
    response.append(buffer.data(), static_cast<size_t>(count));
  }

  return response;
}

class RunningWebService final {
 public:
  explicit RunningWebService(foundry_local::Manager& manager) : manager_(manager) {
    manager_.StartWebService();
  }

  ~RunningWebService() {
    try {
      manager_.StopWebService();
    } catch (...) {
    }
  }

  RunningWebService(const RunningWebService&) = delete;
  RunningWebService& operator=(const RunningWebService&) = delete;

 private:
  foundry_local::Manager& manager_;
};

}  // namespace

int main() {
  using namespace foundry_local;

  try {
    Configuration configuration{std::string(sample::kAppName)};
    configuration.AddWebServiceEndpoint("http://127.0.0.1:0");
    Manager manager(std::move(configuration));
    sample::RegisterExecutionProviders(manager);

    auto model = sample::GetModel(manager.GetCatalog(), "qwen2.5-0.5b");
    sample::LoadedModel loaded_model(*model);
    {
      RunningWebService service(manager);
      const auto endpoints = manager.GetWebServiceEndpoints();
      if (endpoints.empty()) {
        throw std::runtime_error("The web service did not publish an endpoint");
      }

      std::cout << "Web service: " << endpoints.front() << "\n";
      const auto response = PostChatCompletion(endpoints.front(), std::string(model->GetInfo().Id()));
      std::cout << response << "\n";
    }
    std::cout << "Web service stopped; model will now unload.\n";
  } catch (const Error& error) {
    std::cerr << "Foundry Local error [" << error.Code() << "]: " << error.what() << "\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
