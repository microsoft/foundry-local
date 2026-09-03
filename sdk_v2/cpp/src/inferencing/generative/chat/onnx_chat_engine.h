// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/search_options.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

struct OgaEngine;
struct OgaEngineEventBuffer;
struct OgaRequest;

namespace fl {

class GenAIModelInstance;
struct ToolCallContext;

/// Owns one ORT GenAI Engine and serializes every Engine operation onto its owner thread.
class OnnxChatEngine {
 public:
  struct TurnResult {
    uint64_t prompt_tokens = 0;
    uint64_t generated_tokens = 0;
    uint64_t cached_prompt_tokens = 0;
    uint32_t finish_reason = 0;
  };

  class Conversation {
   public:
    Conversation(const Conversation&) = delete;
    Conversation& operator=(const Conversation&) = delete;

   private:
    friend class OnnxChatEngine;
    Conversation() = default;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<int32_t> tokens;
    std::exception_ptr error;
    TurnResult result;
    uint64_t turn_id = 0;
    size_t sequence_length = 0;
    bool turn_finished = true;
    bool closed = false;
  };

  explicit OnnxChatEngine(GenAIModelInstance& model);
  ~OnnxChatEngine();

  OnnxChatEngine(const OnnxChatEngine&) = delete;
  OnnxChatEngine& operator=(const OnnxChatEngine&) = delete;

  std::shared_ptr<Conversation> CreateConversation(const SearchOptions& options,
                                                   const ToolCallContext& tool_ctx,
                                                   int input_token_count);
  uint64_t BeginTurn(const std::shared_ptr<Conversation>& conversation,
                     std::span<const int32_t> input_ids,
                     std::optional<int> max_output_tokens);
  std::optional<int32_t> WaitForToken(const std::shared_ptr<Conversation>& conversation);
  bool IsTurnFinished(const std::shared_ptr<Conversation>& conversation) const;
  TurnResult GetTurnResult(const std::shared_ptr<Conversation>& conversation) const;
  size_t SequenceLength(const std::shared_ptr<Conversation>& conversation) const;
  void Cancel(const std::shared_ptr<Conversation>& conversation);
  void Close(const std::shared_ptr<Conversation>& conversation);

 private:
  struct NativeConversation;
  struct PendingCommand {
    std::function<void()> run;
    std::function<void(std::exception_ptr)> fail;
  };

  void Enqueue(std::function<void()> command, std::function<void(std::exception_ptr)> fail);
  void WorkerLoop(std::promise<void> initialized);
  void RouteEvents();
  void FailAll(std::exception_ptr error);
  NativeConversation& FindNative(const std::shared_ptr<Conversation>& conversation);

  GenAIModelInstance& model_;
  mutable std::mutex command_mutex_;
  std::condition_variable command_cv_;
  std::deque<PendingCommand> commands_;
  std::exception_ptr fatal_error_;
  bool stopping_ = false;
  std::thread worker_;

  // Owner-thread-only state. WorkerLoop clears these before it exits.
  std::unique_ptr<OgaEngine> engine_;
  std::unique_ptr<OgaEngineEventBuffer> event_buffer_;
  std::unordered_map<Conversation*, std::unique_ptr<NativeConversation>> conversations_;
};

}  // namespace fl
