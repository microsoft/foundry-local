// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/onnx_chat_engine.h"

#include "exception.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <ort_genai.h>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace fl {

struct OnnxChatEngine::NativeConversation {
  std::unique_ptr<OgaRequest> request;
  std::shared_ptr<Conversation> state;
};

OnnxChatEngine::OnnxChatEngine(GenAIModelInstance& model) : model_(model) {
  std::promise<void> initialized;
  auto ready = initialized.get_future();
  worker_ = std::thread(&OnnxChatEngine::WorkerLoop, this, std::move(initialized));
  try {
    ready.get();
  } catch (...) {
    if (worker_.joinable()) {
      worker_.join();
    }
    throw;
  }
}

OnnxChatEngine::~OnnxChatEngine() {
  {
    std::lock_guard<std::mutex> lock(command_mutex_);
    stopping_ = true;
  }
  command_cv_.notify_one();

  if (worker_.joinable()) {
    worker_.join();
  }
}

std::shared_ptr<OnnxChatEngine::Conversation> OnnxChatEngine::CreateConversation(
    const SearchOptions& options, const ToolCallContext& tool_ctx, int input_token_count) {
  auto conversation = std::shared_ptr<Conversation>(new Conversation());
  auto completion = std::make_shared<std::promise<void>>();
  auto ready = completion->get_future();

  Enqueue(
      [this, conversation, options, tool_ctx, input_token_count, completion]() {
        auto params = OgaGeneratorParams::Create(model_.GetOgaModel());
        ApplySearchOptions(options, input_token_count, model_.GetGenAIConfig(), *params, model_.EP(),
                           /*use_full_context=*/true);
        ApplyGuidanceOptions(tool_ctx, *params);
        auto request = engine_->CreateRequest(*params);
        conversations_.emplace(conversation.get(),
                               std::make_unique<NativeConversation>(
                                   NativeConversation{std::move(request), conversation}));
        completion->set_value();
      },
      [completion](std::exception_ptr error) { completion->set_exception(error); });

  ready.get();
  return conversation;
}

uint64_t OnnxChatEngine::BeginTurn(const std::shared_ptr<Conversation>& conversation,
                                   std::span<const int32_t> input_ids,
                                   std::optional<int> max_output_tokens) {
  if (input_ids.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Engine turn input must not be empty");
  }

  auto tokens = std::vector<int32_t>(input_ids.begin(), input_ids.end());
  auto completion = std::make_shared<std::promise<uint64_t>>();
  auto ready = completion->get_future();

  Enqueue(
      [this, conversation, tokens = std::move(tokens), max_output_tokens, completion]() {
        auto& native = FindNative(conversation);
        std::unique_ptr<OgaTurnOptions> options;
        if (max_output_tokens.has_value()) {
          options = native.request->CreateTurnOptions();
          options->SetMaxGeneratedTokens(static_cast<uint64_t>(*max_output_tokens));
        }

        {
          std::lock_guard<std::mutex> lock(conversation->mutex);
          if (!conversation->turn_finished) {
            throw std::runtime_error("Cannot begin an Engine turn while another turn is active.");
          }
          conversation->tokens.clear();
          conversation->error = nullptr;
          conversation->result = {};
          conversation->turn_finished = false;
        }

        const uint64_t turn_id = native.request->BeginTurn(tokens.data(), tokens.size(), options.get());
        {
          std::lock_guard<std::mutex> lock(conversation->mutex);
          conversation->turn_id = turn_id;
          conversation->sequence_length += tokens.size();
        }
        completion->set_value(turn_id);
      },
      [conversation, completion](std::exception_ptr error) {
        {
          std::lock_guard<std::mutex> lock(conversation->mutex);
          conversation->error = error;
          conversation->turn_finished = true;
        }
        conversation->cv.notify_all();
        completion->set_exception(error);
      });

  return ready.get();
}

std::optional<int32_t> OnnxChatEngine::WaitForToken(const std::shared_ptr<Conversation>& conversation) {
  std::unique_lock<std::mutex> lock(conversation->mutex);
  conversation->cv.wait(lock, [&]() {
    return !conversation->tokens.empty() || conversation->turn_finished || conversation->error;
  });

  if (conversation->error) {
    std::rethrow_exception(conversation->error);
  }
  if (conversation->tokens.empty()) {
    return std::nullopt;
  }

  const int32_t token = conversation->tokens.front();
  conversation->tokens.pop_front();
  return token;
}

bool OnnxChatEngine::IsTurnFinished(const std::shared_ptr<Conversation>& conversation) const {
  std::lock_guard<std::mutex> lock(conversation->mutex);
  return conversation->turn_finished && conversation->tokens.empty();
}

OnnxChatEngine::TurnResult OnnxChatEngine::GetTurnResult(
    const std::shared_ptr<Conversation>& conversation) const {
  std::unique_lock<std::mutex> lock(conversation->mutex);
  conversation->cv.wait(lock, [&]() { return conversation->turn_finished || conversation->error; });
  if (conversation->error) {
    std::rethrow_exception(conversation->error);
  }
  return conversation->result;
}

size_t OnnxChatEngine::SequenceLength(const std::shared_ptr<Conversation>& conversation) const {
  std::lock_guard<std::mutex> lock(conversation->mutex);
  return conversation->sequence_length;
}

void OnnxChatEngine::Cancel(const std::shared_ptr<Conversation>& conversation) {
  Enqueue(
      [this, conversation]() {
        auto& native = FindNative(conversation);
        uint64_t turn_id;
        {
          std::lock_guard<std::mutex> lock(conversation->mutex);
          turn_id = conversation->turn_id;
        }
        if (turn_id != 0) {
          native.request->CancelTurn(turn_id);
        }
      },
      [conversation](std::exception_ptr error) {
        std::lock_guard<std::mutex> lock(conversation->mutex);
        conversation->error = error;
        conversation->turn_finished = true;
        conversation->cv.notify_all();
      });
}

void OnnxChatEngine::Close(const std::shared_ptr<Conversation>& conversation) {
  auto completion = std::make_shared<std::promise<void>>();
  auto ready = completion->get_future();
  Enqueue(
      [this, conversation, completion]() {
        auto it = conversations_.find(conversation.get());
        if (it != conversations_.end()) {
          it->second->request->Close();
          conversations_.erase(it);
        }
        {
          std::lock_guard<std::mutex> lock(conversation->mutex);
          conversation->closed = true;
          conversation->turn_finished = true;
        }
        conversation->cv.notify_all();
        completion->set_value();
      },
      [completion](std::exception_ptr error) { completion->set_exception(error); });
  ready.get();
}

void OnnxChatEngine::Enqueue(std::function<void()> command,
                             std::function<void(std::exception_ptr)> fail) {
  std::exception_ptr error;
  {
    std::lock_guard<std::mutex> lock(command_mutex_);
    error = fatal_error_;
    if (stopping_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine dispatcher is shutting down");
    }
    if (!error) {
      commands_.push_back({std::move(command), std::move(fail)});
    }
  }

  if (error) {
    fail(error);
    return;
  }
  command_cv_.notify_one();
}

void OnnxChatEngine::WorkerLoop(std::promise<void> initialized) {
  try {
    engine_ = OgaEngine::Create(model_.GetOgaModel());
    event_buffer_ = engine_->CreateEventBuffer(model_.GetGenAIConfig().EngineMaxBatchSize().value_or(1) * 2);
    initialized.set_value();
  } catch (...) {
    event_buffer_.reset();
    engine_.reset();
    initialized.set_exception(std::current_exception());
    return;
  }

  try {
    while (true) {
      std::deque<PendingCommand> commands;
      {
        std::unique_lock<std::mutex> lock(command_mutex_);
        if (commands_.empty() && !engine_->HasPendingRequests() && !stopping_) {
          command_cv_.wait(lock, [&]() { return stopping_ || !commands_.empty(); });
        }
        commands.swap(commands_);
        if (stopping_ && commands.empty() && !engine_->HasPendingRequests()) {
          break;
        }
      }

      for (auto& command : commands) {
        try {
          command.run();
        } catch (...) {
          command.fail(std::current_exception());
        }
      }
      if (engine_->HasPendingRequests()) {
        RouteEvents();
      }
    }
  } catch (...) {
    auto error = std::current_exception();
    std::deque<PendingCommand> commands;
    {
      std::lock_guard<std::mutex> lock(command_mutex_);
      fatal_error_ = error;
      stopping_ = true;
      commands.swap(commands_);
    }

    FailAll(error);
    for (auto& command : commands) {
      command.fail(error);
    }
  }

  conversations_.clear();
  event_buffer_.reset();
  engine_.reset();
}

void OnnxChatEngine::RouteEvents() {
  engine_->Run(*event_buffer_);
  for (size_t i = 0; i < event_buffer_->Count(); ++i) {
    const auto* event = event_buffer_->Get(i);
    const auto flags = event->Flags();
    const auto request = event->Request();
    if (!request) {
      if ((flags & OgaEngineEventFlag_Failed) != 0) {
        throw std::runtime_error("ORT GenAI Engine failed with error code " +
                                 std::to_string(event->ErrorCode()));
      }
      if ((flags & OgaEngineEventFlag_CapacityBlocked) != 0 && EvictDormantConversation()) {
        consecutive_retry_events_ = 0;
        continue;
      }
      if ((flags & (OgaEngineEventFlag_CapacityBlocked | OgaEngineEventFlag_Retryable)) != 0) {
        constexpr size_t kMaxConsecutiveRetries = 100;
        if (++consecutive_retry_events_ > kMaxConsecutiveRetries) {
          throw std::runtime_error("ORT GenAI Engine made no progress after " +
                                   std::to_string(kMaxConsecutiveRetries) +
                                   " retryable events; last error code " +
                                   std::to_string(event->ErrorCode()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      throw std::runtime_error("ORT GenAI Engine returned an invalid request-less event");
    }
    consecutive_retry_events_ = 0;

    auto it = std::find_if(conversations_.begin(), conversations_.end(), [&](const auto& entry) {
      return entry.second->request.get() == &request->get();
    });
    if (it == conversations_.end()) {
      continue;
    }

    auto& conversation = it->second->state;
    {
      std::lock_guard<std::mutex> lock(conversation->mutex);
      if ((flags & OgaEngineEventFlag_Token) != 0) {
        conversation->tokens.push_back(event->Token());
        ++conversation->sequence_length;
      }
      if ((flags & OgaEngineEventFlag_TurnFinished) != 0) {
        const auto& usage = event->Usage();
        conversation->result.prompt_tokens = usage.PromptTokens();
        conversation->result.generated_tokens = usage.GeneratedTokens();
        conversation->result.cached_prompt_tokens = usage.CachedPromptTokens();
        conversation->result.finish_reason = event->FinishReason();
        conversation->turn_finished = true;
      }
      if ((flags & OgaEngineEventFlag_Failed) != 0) {
        conversation->error = std::make_exception_ptr(
            std::runtime_error("ORT GenAI Engine request failed with error code " +
                               std::to_string(event->ErrorCode())));
        conversation->turn_finished = true;
      }
    }
    conversation->cv.notify_all();
  }
}

bool OnnxChatEngine::EvictDormantConversation() {
  for (auto it = conversations_.begin(); it != conversations_.end(); ++it) {
    auto conversation = it->second->state;
    {
      std::lock_guard<std::mutex> lock(conversation->mutex);
      if (!conversation->turn_finished) {
        continue;
      }
      conversation->closed = true;
    }

    it->second->request->Close();
    conversations_.erase(it);
    conversation->cv.notify_all();
    return true;
  }
  return false;
}

void OnnxChatEngine::FailAll(std::exception_ptr error) {
  for (auto& [_, native] : conversations_) {
    {
      std::lock_guard<std::mutex> lock(native->state->mutex);
      native->state->error = error;
      native->state->turn_finished = true;
    }
    native->state->cv.notify_all();
  }
}

OnnxChatEngine::NativeConversation& OnnxChatEngine::FindNative(
    const std::shared_ptr<Conversation>& conversation) {
  auto it = conversations_.find(conversation.get());
  if (it == conversations_.end()) {
    throw ConversationEvictedError();
  }
  return *it->second;
}

}  // namespace fl
