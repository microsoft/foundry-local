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
#include <vector>

namespace fl {

namespace {

// Only options the caller expressed are set. Upstream treats an unset turn option as "use the model-configured
// default for this Turn", so forwarding a Foundry-invented default would silently override model policy — and an
// explicit do_sample=true is rejected outright when the model's own defaults still resolve the turn to greedy.
void ApplyEngineTurnOptions(const EngineTurnOptionsPlan& plan, OgaTurnOptions& options) {
  if (plan.max_generated_tokens.has_value()) {
    options.SetMaxGeneratedTokens(static_cast<uint64_t>(*plan.max_generated_tokens));
  }

  if (plan.sampling.do_sample.has_value()) {
    options.SetDoSample(*plan.sampling.do_sample);
  }

  if (plan.sampling.temperature.has_value()) {
    options.SetTemperature(*plan.sampling.temperature);
  }

  if (plan.sampling.top_p.has_value()) {
    options.SetTopP(*plan.sampling.top_p);
  }

  if (plan.sampling.top_k.has_value()) {
    options.SetTopK(*plan.sampling.top_k);
  }

  if (plan.seed.has_value()) {
    options.SetSeed(static_cast<uint64_t>(*plan.seed));
  }

  if (!plan.stop_sequences.empty()) {
    std::vector<const char*> raw_stop_strings;
    raw_stop_strings.reserve(plan.stop_sequences.size());
    for (const auto& stop : plan.stop_sequences) {
      raw_stop_strings.push_back(stop.c_str());
    }

    auto stop_strings = OgaStringArray::Create(raw_stop_strings.data(), raw_stop_strings.size());
    options.SetStopStrings(*stop_strings);
  }

  if (plan.guidance.has_value()) {
    try {
      options.SetGuidance(plan.guidance->type.c_str(), plan.guidance->data.c_str());
    } catch (const std::runtime_error& e) {
      if (plan.guidance->user_specified) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "failed to apply requested response guidance: " + std::string(e.what()));
      }

      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "failed to apply required tool-call guidance: " + std::string(e.what()));
    }
  }
}

}  // namespace

struct OnnxChatEngine::NativeConversation {
  std::unique_ptr<OgaRequest> request;
  std::shared_ptr<Conversation> state;
  bool admitted = false;
};

OnnxChatEngine::OnnxChatEngine(GenAIModelInstance& model, std::chrono::milliseconds capacity_wait_timeout)
    : model_(model), capacity_wait_timeout_(capacity_wait_timeout) {
  if (capacity_wait_timeout_ <= std::chrono::milliseconds::zero()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Engine capacity wait timeout must be positive");
  }

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
    const SearchOptions&, const ToolCallContext&, int) {
  auto conversation = std::shared_ptr<Conversation>(new Conversation());
  auto completion = std::make_shared<std::promise<void>>();
  auto ready = completion->get_future();

  Enqueue(
      [this, conversation, completion]() {
        auto request_options = OgaRequestOptions::Create();
        request_options->SetMaxSessionTokens(static_cast<uint64_t>(GetModelMaxContextLength(model_.GetGenAIConfig())));

        auto request = engine_->CreateRequest(request_options.get());
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
                                   const SearchOptions& options,
                                   const ToolCallContext& tool_ctx,
                                   bool prompt_opens_reasoning) {
  if (input_ids.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Engine turn input must not be empty");
  }

  auto tokens = std::vector<int32_t>(input_ids.begin(), input_ids.end());
  auto completion = std::make_shared<std::promise<uint64_t>>();
  auto ready = completion->get_future();

  Enqueue(
      [this, conversation, tokens = std::move(tokens), options, tool_ctx, prompt_opens_reasoning, completion]() {
        auto& native = FindNative(conversation);
        auto turn_options = native.request->CreateTurnOptions();
        size_t existing_tokens = 0;
        std::vector<int32_t> resident_tokens;

        {
          std::lock_guard<std::mutex> lock(conversation->mutex);
          if (!conversation->turn_finished) {
            throw std::runtime_error("Cannot begin an Engine turn while another turn is active.");
          }
          existing_tokens = conversation->resident_tokens.size();
          resident_tokens = conversation->resident_tokens;
          resident_tokens.insert(resident_tokens.end(), tokens.begin(), tokens.end());
          conversation->tokens.clear();
          conversation->error = nullptr;
          conversation->result = {};
          conversation->turn_finished = false;
          conversation->turn_has_progress = false;
        }

        auto plan = BuildEngineTurnOptionsPlan(options, tool_ctx, model_.GetGenAIConfig().GetChatBackendKind(),
                                               prompt_opens_reasoning);
        if (plan.max_generated_tokens.has_value()) {
          // Validate only the limit the caller requested. When it is absent, leave the turn uncapped and let OGA
          // enforce the Request's model-context session limit.
          const uint64_t total_required =
              static_cast<uint64_t>(existing_tokens) + static_cast<uint64_t>(tokens.size()) +
              static_cast<uint64_t>(*plan.max_generated_tokens);
          const uint64_t model_max_tokens = static_cast<uint64_t>(GetModelMaxContextLength(model_.GetGenAIConfig()));
          if (total_required > model_max_tokens) {
            FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                     "request requires " + std::to_string(total_required) + " total tokens (" +
                         std::to_string(existing_tokens) + " existing + " + std::to_string(tokens.size()) +
                         " input + " + std::to_string(*plan.max_generated_tokens) +
                         " output), which exceeds the model's maximum context length of " +
                         std::to_string(model_max_tokens) + " tokens");
          }
        }

        ApplyEngineTurnOptions(plan, *turn_options);

        const uint64_t turn_id = native.request->BeginTurn(tokens.data(), tokens.size(), turn_options.get());
        {
          std::lock_guard<std::mutex> lock(conversation->mutex);
          conversation->turn_id = turn_id;
          conversation->resident_tokens = std::move(resident_tokens);
          conversation->turn_started_at = std::chrono::steady_clock::now();
          conversation->last_activity = conversation->turn_started_at;
          conversation->admission_sequence = next_admission_sequence_++;
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
  return conversation->resident_tokens.size();
}

std::vector<int32_t> OnnxChatEngine::ResidentTokens(
    const std::shared_ptr<Conversation>& conversation) const {
  std::lock_guard<std::mutex> lock(conversation->mutex);
  return conversation->resident_tokens;
}

void OnnxChatEngine::Cancel(const std::shared_ptr<Conversation>& conversation) {
  Enqueue(
      [this, conversation]() {
        uint64_t turn_id;
        {
          std::lock_guard<std::mutex> lock(conversation->mutex);
          if (conversation->turn_finished || conversation->turn_id == 0) {
            return;
          }
          turn_id = conversation->turn_id;
        }

        auto& native = FindNative(conversation);
        native.request->CancelTurn(turn_id);
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
    // Drain an ordinary full decode batch in one call. OGA retains speculative or fatal overflow for later runs.
    event_buffer_ = engine_->CreateEventBuffer(model_.GetGenAIConfig().EngineMaxBatchSize().value_or(1));
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
        continue;
      }
      if ((flags & (OgaEngineEventFlag_CapacityBlocked | OgaEngineEventFlag_Retryable)) != 0) {
        if ((flags & OgaEngineEventFlag_CapacityBlocked) != 0 && ExpireCapacityBlockedConversation()) {
          continue;
        }

        // Capacity pressure is transient while resident requests are active. Leave the request queued in OGA and
        // return to the dispatcher so cancellation and close commands can still make progress.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      throw std::runtime_error("ORT GenAI Engine returned an invalid request-less event");
    }
    auto it = std::find_if(conversations_.begin(), conversations_.end(), [&](const auto& entry) {
      return entry.second->request.get() == &request->get();
    });
    if (it == conversations_.end()) {
      continue;
    }

    auto& conversation = it->second->state;
    {
      std::lock_guard<std::mutex> lock(conversation->mutex);
      conversation->turn_has_progress = true;
      conversation->last_activity = std::chrono::steady_clock::now();
      if ((flags & OgaEngineEventFlag_Token) != 0) {
        it->second->admitted = true;
        const auto token = event->Token();
        conversation->tokens.push_back(token);
        conversation->resident_tokens.push_back(token);
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

    if ((flags & OgaEngineEventFlag_Failed) != 0) {
      it->second->request->Close();
      conversations_.erase(it);
    }
  }

  // A full resident batch can keep emitting tokens without a CapacityBlocked event. Service waiting admissions
  // between those steps too, without confusing an in-progress initial prefill with a capacity-blocked request.
  size_t resident_count = 0;
  bool has_waiting_admission = false;
  for (const auto& [_, native] : conversations_) {
    if (native->admitted) {
      ++resident_count;
    } else {
      std::lock_guard<std::mutex> lock(native->state->mutex);
      has_waiting_admission |= native->state->turn_id != 0 && !native->state->turn_finished;
    }
  }

  if (has_waiting_admission && resident_count >= model_.GetGenAIConfig().EngineMaxBatchSize().value_or(1)) {
    if (!EvictDormantConversation()) {
      ExpireCapacityBlockedConversation(/*new_admissions_only=*/true);
    }
  }
}

bool OnnxChatEngine::EvictDormantConversation() {
  auto victim = conversations_.end();
  auto oldest_activity = std::chrono::steady_clock::time_point::max();
  for (auto it = conversations_.begin(); it != conversations_.end(); ++it) {
    const auto& conversation = it->second->state;
    std::lock_guard<std::mutex> lock(conversation->mutex);
    if (conversation->turn_finished && conversation->turn_id != 0 &&
        conversation->last_activity < oldest_activity) {
      victim = it;
      oldest_activity = conversation->last_activity;
    }
  }

  if (victim == conversations_.end()) {
    return false;
  }

  auto conversation = victim->second->state;
  {
    std::lock_guard<std::mutex> lock(conversation->mutex);
    conversation->closed = true;
  }

  victim->second->request->Close();
  conversations_.erase(victim);
  conversation->cv.notify_all();
  return true;
}

bool OnnxChatEngine::ExpireCapacityBlockedConversation(bool new_admissions_only) {
  auto candidate = conversations_.end();
  uint64_t newest_admission = 0;
  const auto now = std::chrono::steady_clock::now();
  for (auto it = conversations_.begin(); it != conversations_.end(); ++it) {
    if (new_admissions_only && it->second->admitted) {
      continue;
    }

    const auto& conversation = it->second->state;
    std::lock_guard<std::mutex> lock(conversation->mutex);
    if (!conversation->turn_finished && !conversation->turn_has_progress &&
        now - conversation->turn_started_at >= capacity_wait_timeout_ &&
        conversation->admission_sequence > newest_admission) {
      candidate = it;
      newest_admission = conversation->admission_sequence;
    }
  }

  if (candidate == conversations_.end()) {
    return false;
  }

  auto conversation = candidate->second->state;
  uint64_t turn_id;
  {
    std::lock_guard<std::mutex> lock(conversation->mutex);
    turn_id = conversation->turn_id;
  }

  candidate->second->request->CancelTurn(turn_id);
  candidate->second->request->Close();
  conversations_.erase(candidate);

  {
    std::lock_guard<std::mutex> lock(conversation->mutex);
    conversation->error = std::make_exception_ptr(std::runtime_error(
        "ORT GenAI Engine capacity remained unavailable for " +
        std::to_string(capacity_wait_timeout_.count()) + " ms"));
    conversation->turn_finished = true;
    conversation->closed = true;
  }
  conversation->cv.notify_all();
  return true;
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
