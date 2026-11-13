// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/harmony_decoder.h"

#include "exception.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace fl {
namespace {

bool HasRecipientField(std::string_view header) {
  size_t position = 0;
  while ((position = header.find("to=", position)) != std::string_view::npos) {
    if (position == 0 || std::string_view(" \t\r\n\x01\x02").find(header[position - 1]) != std::string_view::npos) {
      return true;
    }
    position += 3;
  }
  return false;
}

}  // namespace

HarmonyDecoder::HarmonyDecoder(HarmonyProtocol protocol, std::shared_ptr<const ToolCallPlan> plan,
                               const PreparedChatPrompt& prompt,
                               std::unordered_set<std::string> raw_tool_names)
    : protocol_(std::move(protocol)), plan_(std::move(plan)), raw_tool_names_(std::move(raw_tool_names)) {
  if (!prompt.harmony_continuation) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Harmony decoding requires an explicit prepared continuation");
  }
  const auto& continuation = *prompt.harmony_continuation;
  if (continuation.header_tokens > prompt.token_ids.size() || continuation.header_bytes > prompt.prompt.size()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Invalid prepared Harmony header extent");
  }
  header_ = continuation.header;
  checkpoint_.prompt = prompt.prompt;
  checkpoint_.token_ids = prompt.token_ids;
  message_token_offset_ = prompt.token_ids.size() - continuation.header_tokens;
  message_byte_offset_ = prompt.prompt.size() - continuation.header_bytes;
}

HarmonyDecoder::Output HarmonyDecoder::Fail(std::string error) {
  failed_ = true;
  arguments_.clear();
  return {{}, true, std::move(error)};
}

void HarmonyDecoder::CaptureCheckpoint() {
  if (!checkpoint_captured_ && plan_) {
    checkpoint_.token_ids.resize(message_token_offset_);
    checkpoint_.prompt.resize(message_byte_offset_);
    checkpoint_captured_ = true;
  }
}

void HarmonyDecoder::RecordToken(int32_t token, const std::string& text) {
  if (checkpoint_captured_ || !plan_) {
    return;
  }
  if (token == protocol_.start) {
    message_token_offset_ = checkpoint_.token_ids.size();
    message_byte_offset_ = checkpoint_.prompt.size();
  }
  checkpoint_.token_ids.push_back(token);
  if (token == protocol_.start)
    checkpoint_.prompt += "<|start|>";
  else if (token == protocol_.channel)
    checkpoint_.prompt += "<|channel|>";
  else if (token == protocol_.message)
    checkpoint_.prompt += "<|message|>";
  else if (token == protocol_.end)
    checkpoint_.prompt += "<|end|>";
  else if (token == protocol_.format)
    checkpoint_.prompt += "<|constrain|>";
  else
    checkpoint_.prompt += text;
}

void HarmonyDecoder::ParseHeader() {
  if (HasRecipientField(header_)) {
    CaptureCheckpoint();
  }
  if (std::count(header_.begin(), header_.end(), '\x01') != 1 ||
      std::count(header_.begin(), header_.end(), '\x02') > 1) {
    throw std::runtime_error("Harmony header requires exactly one channel");
  }
  const auto channel_position = header_.find('\x01');
  const auto format_position = header_.find('\x02');
  if (format_position != std::string::npos && format_position < channel_position) {
    throw std::runtime_error("Harmony format metadata must follow the channel");
  }
  std::istringstream prefix(header_.substr(0, channel_position));
  std::string field;
  if (!(prefix >> field) || field != "assistant") {
    throw std::runtime_error("Harmony output participant must be assistant");
  }
  channel_.clear();
  recipient_.clear();
  const auto read_recipient = [&](const std::string& value) {
    if (!recipient_.empty() || !value.starts_with("to=functions.") || value.size() == 13) {
      throw std::runtime_error("Invalid Harmony tool recipient");
    }
    recipient_ = value.substr(13);
  };
  while (prefix >> field) read_recipient(field);
  std::istringstream channel(header_.substr(channel_position + 1,
                                            format_position == std::string::npos
                                                ? std::string::npos
                                                : format_position - channel_position - 1));
  if (!(channel >> channel_) ||
      (channel_ != "analysis" && channel_ != "commentary" && channel_ != "final")) {
    throw std::runtime_error("Unsupported Harmony channel");
  }
  bool has_format = false;
  while (channel >> field) {
    if (field == "json" && !recipient_.empty() && format_position == std::string::npos) {
      has_format = true;
      if (channel >> field) throw std::runtime_error("Unexpected field after Harmony format metadata");
    } else {
      read_recipient(field);
    }
  }
  if (format_position != std::string::npos) {
    has_format = true;
    std::istringstream format(header_.substr(format_position + 1));
    if (!(format >> field) || field != "json" || (format >> field)) {
      throw std::runtime_error("Unsupported Harmony format metadata");
    }
  }
  if ((!recipient_.empty() && channel_ != "commentary") ||
      (has_format && recipient_.empty()) || (channel_ == "analysis" && visible_started_)) {
    throw std::runtime_error("Unsupported Harmony assistant-channel transition");
  }
  if (has_format && raw_tool_names_.contains(recipient_)) {
    throw std::runtime_error("Native custom tools require a raw body without JSON metadata");
  }
  visible_started_ |= channel_ != "analysis";
}

HarmonyDecoder::Output HarmonyDecoder::Push(std::optional<int32_t> token, const std::string& text) {
  if (failed_) return {};
  if (!token) return text.empty() ? Output{} : Fail("Harmony decoding requires token identity");
  if (phase_ == Phase::kDone) {
    return token == ending_ ? Output{} : Fail("Output after Harmony turn completion");
  }
  RecordToken(*token, text);
  try {
    if (*token == protocol_.start) {
      if (phase_ != Phase::kBetween) return Fail("Harmony message started before the previous message ended");
      header_.clear();
      phase_ = Phase::kHeader;
    } else if (*token == protocol_.channel || *token == protocol_.format) {
      if (phase_ != Phase::kHeader) return Fail("Harmony header control in a message body");
      header_ += *token == protocol_.channel ? '\x01' : '\x02';
    } else if (*token == protocol_.message) {
      if (phase_ != Phase::kHeader) return Fail("Unexpected Harmony message-body boundary");
      ParseHeader();
      phase_ = Phase::kBody;
    } else if (*token == protocol_.end) {
      if (phase_ != Phase::kBody || !recipient_.empty() || channel_ == "final") {
        return Fail("Harmony message completion cannot authorize a tool call or final answer");
      }
      phase_ = Phase::kBetween;
    } else if (*token == protocol_.handoff) {
      if (phase_ != Phase::kBody || recipient_.empty() || !plan_) {
        return Fail("Harmony handoff requires a selected declared tool");
      }
      ParsedToolCall call;
      call.name = recipient_;
      call.arguments = raw_tool_names_.contains(recipient_)
                           ? nlohmann::json{{"input", arguments_}}.dump()
                           : arguments_;
      call.argument_source = call.arguments;
      plan_->ValidateBatch({call});
      call.id = GenerateToolCallId();
      phase_ = Phase::kDone;
      ending_ = token;
      return {{std::move(call)}, false, {}};
    } else if (*token == protocol_.answer) {
      if (phase_ != Phase::kBody || channel_ != "final" || !recipient_.empty()) {
        return Fail("Harmony answer completion requires a final message");
      }
      phase_ = Phase::kDone;
      ending_ = token;
    } else if (phase_ == Phase::kHeader) {
      if (HasRecipientField(header_ + text.substr(0, 1025))) {
        CaptureCheckpoint();
      }
      if (text.find_first_of("\x01\x02") != std::string::npos) {
        return Fail("Ordinary header bytes cannot manufacture Harmony metadata controls");
      }
      if (header_.size() + text.size() > 1024) return Fail("Harmony header exceeds its size limit");
      header_ += text;
    } else if (phase_ == Phase::kBody && !recipient_.empty()) {
      if (arguments_.size() + text.size() > ToolCallPlan::kMaxArgumentBytes) {
        return Fail("Harmony argument body exceeds its size limit");
      }
      arguments_ += text;
    } else if (phase_ == Phase::kBody) {
      const auto type = channel_ == "analysis" ? FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING
                                               : FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT;
      reasoning_tokens_ += channel_ == "analysis";
      if (!text.empty()) return {{ReasoningStreamSplitter::Segment{text, type, token}}, false, {}};
    } else if (text.find_first_not_of(" \t\r\n") != std::string::npos) {
      return Fail("Unexpected bytes between Harmony messages");
    }
  } catch (const std::exception& error) {
    return Fail(error.what());
  }
  return {};
}

HarmonyDecoder::Output HarmonyDecoder::Finish(std::optional<int32_t> terminal_token, bool natural_end) {
  if (failed_) return {};
  if (natural_end && terminal_token &&
      (*terminal_token == protocol_.handoff || *terminal_token == protocol_.answer)) {
    return Push(terminal_token, {});
  }
  if (phase_ == Phase::kDone) return {};
  if (natural_end) return Fail("Incomplete Harmony turn");
  arguments_.clear();
  recipient_.clear();
  phase_ = Phase::kDone;
  return {};
}

PreparedChatPrompt HarmonyDecoder::TakeRecoveryPrompt() {
  if (!checkpoint_captured_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Harmony recovery has no selected-call checkpoint");
  }
  checkpoint_.prompt_token_count = static_cast<int64_t>(checkpoint_.token_ids.size());
  return std::move(checkpoint_);
}

}  // namespace fl
