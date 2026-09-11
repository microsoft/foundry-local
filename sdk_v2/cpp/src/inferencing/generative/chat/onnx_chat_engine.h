// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/search_options.h"

#include <algorithm>
#include <chrono>
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
#include <stdexcept>
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
  static constexpr std::chrono::seconds kDefaultCapacityWaitTimeout{30};

  class ConversationEvictedError : public std::runtime_error {
   public:
    ConversationEvictedError() : std::runtime_error("Engine conversation was evicted for capacity") {}
  };

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
    std::vector<int32_t> resident_tokens;
    std::exception_ptr error;
    TurnResult result;
    uint64_t turn_id = 0;
    std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point turn_started_at;
    uint64_t admission_sequence = 0;
    bool turn_has_progress = false;
    bool turn_finished = true;
    bool closed = false;
  };

  explicit OnnxChatEngine(
      GenAIModelInstance& model,
      std::chrono::milliseconds capacity_wait_timeout = kDefaultCapacityWaitTimeout);
  ~OnnxChatEngine();

  OnnxChatEngine(const OnnxChatEngine&) = delete;
  OnnxChatEngine& operator=(const OnnxChatEngine&) = delete;

  std::shared_ptr<Conversation> CreateConversation(const SearchOptions& options,
                                                   const ToolCallContext& tool_ctx,
                                                   int input_token_count);
  uint64_t BeginTurn(const std::shared_ptr<Conversation>& conversation,
                     std::span<const int32_t> input_ids,
                     const SearchOptions& options,
                     const ToolCallContext& tool_ctx,
                     bool prompt_opens_reasoning);
  std::optional<int32_t> WaitForToken(const std::shared_ptr<Conversation>& conversation);
  bool IsTurnFinished(const std::shared_ptr<Conversation>& conversation) const;
  TurnResult GetTurnResult(const std::shared_ptr<Conversation>& conversation) const;
  size_t SequenceLength(const std::shared_ptr<Conversation>& conversation) const;
  std::vector<int32_t> ResidentTokens(const std::shared_ptr<Conversation>& conversation) const;
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
  bool EvictDormantConversation();
  bool ExpireCapacityBlockedConversation(bool new_admissions_only = false);
  void FailAll(std::exception_ptr error);
  NativeConversation& FindNative(const std::shared_ptr<Conversation>& conversation);

  GenAIModelInstance& model_;
  const std::chrono::milliseconds capacity_wait_timeout_;
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
  uint64_t next_admission_sequence_ = 1;
};

}  // namespace fl
