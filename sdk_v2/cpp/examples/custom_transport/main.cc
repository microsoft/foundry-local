// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_cpp.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct TransportState {
  const flApi* api;
  std::atomic<size_t> references{1};
  std::atomic<size_t> requests{0};
  std::atomic<size_t> operations{0};
  std::atomic<size_t> reads{0};
};

struct Operation {
  TransportState* state;
  std::atomic<bool> canceled{false};
  std::string body = R"({"value":[]})";
  size_t position = 0;
  flHttpHeader headers[2] = {{"content-type", "application/json"}, {"x-example", "custom-transport"}};
};

void FL_API_CALL Retain(void* user_data) noexcept {
  static_cast<TransportState*>(user_data)->references.fetch_add(1);
}

void FL_API_CALL Release(void* user_data) noexcept {
  auto* state = static_cast<TransportState*>(user_data);
  if (state->references.fetch_sub(1) == 1) {
    delete state;
  }
}

flStatus* FL_API_CALL CreateOperation(void* user_data, const flHttpRequestData* request, void** out_operation) noexcept {
  *out_operation = nullptr;
  auto* state = static_cast<TransportState*>(user_data);
  const bool catalog_request =
      std::strcmp(request->method, "POST") == 0 && std::strcmp(request->url, "https://example.invalid/catalog") == 0;
  const bool cancellation_example =
      std::strcmp(request->method, "GET") == 0 && std::strcmp(request->url, "https://example.invalid/model") == 0;
  if (!catalog_request && !cancellation_example) {
    return state->api->Status_Create(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported example request");
  }

  try {
    auto operation = std::make_unique<Operation>();
    operation->state = state;
    state->operations.fetch_add(1);
    *out_operation = operation.release();
    return nullptr;
  } catch (const std::exception&) {
    return state->api->Status_Create(FOUNDRY_LOCAL_ERROR_INTERNAL, "could not allocate example operation");
  }
}

flStatus* FL_API_CALL Send(void* handle, flHttpResponseData* response) noexcept {
  auto& operation = *static_cast<Operation*>(handle);
  if (operation.canceled.load()) {
    return operation.state->api->Status_Create(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "example operation canceled");
  }

  operation.state->requests.fetch_add(1);
  response->version = FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION;
  response->status_code = 200;
  response->headers = operation.headers;
  response->header_count = 2;
  return nullptr;
}

flStatus* FL_API_CALL Read(void* handle, uint8_t* buffer, size_t capacity, size_t* bytes_read) noexcept {
  auto& operation = *static_cast<Operation*>(handle);
  *bytes_read = 0;
  if (operation.canceled.load()) {
    return operation.state->api->Status_Create(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "example operation canceled");
  }

  // Deliberate short reads show that the SDK must consume a stream rather than assume one complete buffer.
  const auto count = std::min({capacity, size_t{3}, operation.body.size() - operation.position});
  std::memcpy(buffer, operation.body.data() + operation.position, count);
  operation.position += count;
  *bytes_read = count;
  operation.state->reads.fetch_add(1);
  return nullptr;
}

void FL_API_CALL Cancel(void* handle) noexcept {
  // This operation has no blocking I/O. A network adapter must also interrupt its socket or pending future.
  static_cast<Operation*>(handle)->canceled.store(true);
}

void FL_API_CALL ReleaseOperation(void* handle) noexcept {
  auto* operation = static_cast<Operation*>(handle);
  operation->state->operations.fetch_sub(1);
  delete operation;
}

struct ReleaseState {
  void operator()(TransportState* state) const noexcept { Release(state); }
};

void Check(const flApi* api, flStatus* status) {
  if (status) {
    const std::string message = api->Status_GetErrorMessage(status);
    api->Status_Release(status);
    throw std::runtime_error(message);
  }
}

void DemonstrateCancellation(const flHttpTransport& transport, const flApi* api) {
  flHttpRequestData request{};
  request.version = FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION;
  request.method = "GET";
  request.url = "https://example.invalid/model";
  void* operation = nullptr;
  Check(api, transport.CreateOperation(transport.user_data, &request, &operation));
  transport.Cancel(operation);
  flHttpResponseData response{};
  response.version = FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION;
  flStatus* status = transport.Send(operation, &response);
  const bool canceled = status && api->Status_GetErrorCode(status) == FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED;
  if (status) {
    api->Status_Release(status);
  }

  transport.ReleaseOperation(operation);
  if (!canceled) {
    throw std::runtime_error("pre-send cancellation did not return the expected error");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const flApi* api = FoundryLocalGetApi(FOUNDRY_LOCAL_API_VERSION);
    if (!api) {
      throw std::runtime_error("the runtime does not support the custom transport API");
    }

    std::unique_ptr<TransportState, ReleaseState> state(new TransportState{api});
    flHttpTransport transport{};
    transport.version = FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION;
    transport.user_data = state.get();
    transport.Retain = Retain;
    transport.Release = Release;
    transport.CreateOperation = CreateOperation;
    transport.Send = Send;
    transport.Read = Read;
    transport.Cancel = Cancel;
    transport.ReleaseOperation = ReleaseOperation;

    // Keep generated sample data beside the executable unless the caller supplies an output directory.
    const auto data_dir = argc > 1 ? std::filesystem::path(argv[1])
                                   : std::filesystem::absolute(argv[0]).parent_path() / "custom-transport-data";
    {
      foundry_local::Configuration config("custom_transport_example");
      config.SetAppDataDir(data_dir.string());
      config.AddCatalogUrl("https://example.invalid/catalog");
      config.SetDisableNonessentialTelemetry(true);
      config.SetModelHttpTransport(&transport);
      foundry_local::Manager manager(std::move(config));
      manager.GetCatalog().GetModels();
    }

    if (state->requests.load() == 0 || state->reads.load() < 2) {
      throw std::runtime_error("the SDK did not consume the custom catalog stream");
    }

    DemonstrateCancellation(transport, api);
    if (state->operations.load() != 0 || state->references.load() != 1) {
      throw std::runtime_error("the SDK did not release its transport ownership");
    }

    std::cout << "Native transport: " << state->requests.load() << " catalog requests, " << state->reads.load()
              << " partial reads, pre-send cancellation, and balanced ownership\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Custom transport example failed: " << error.what() << '\n';
    return 1;
  }
}
