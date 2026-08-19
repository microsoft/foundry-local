// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct OgaGeneratorParams;
struct OgaModel;

namespace fl {

/// Narrow backend boundary for testing EngineHost without loading a model.
class EngineBackend {
 public:
  class Request {
   public:
    virtual ~Request() = default;
  };

  struct ReadyRequest {
    Request* request;
    std::vector<int32_t> tokens;
    bool is_turn_complete;
  };

  virtual ~EngineBackend() = default;

  virtual std::unique_ptr<Request> CreateRequest(OgaGeneratorParams& params,
                                                 std::span<const int32_t> tokens) = 0;
  virtual void Add(Request& request) = 0;
  virtual std::unique_ptr<ReadyRequest> Step() = 0;
  /// The caller-provided max_length remains cumulative across turns; this layer does not rewrite generator parameters.
  virtual void Continue(Request& request, std::span<const int32_t> tokens) = 0;
  virtual void Remove(Request& request) = 0;
};

/// Creates the production backend using only public ort_genai.h APIs.
std::unique_ptr<EngineBackend> CreateOgaEngineBackend(OgaModel& model);

}  // namespace fl
