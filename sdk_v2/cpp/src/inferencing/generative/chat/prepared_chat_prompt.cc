// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/prepared_chat_prompt.h"

#include "exception.h"
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "items/audio_item.h"
#include "items/image_item.h"

#include <ort_genai.h>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <algorithm>

namespace fl {

PreparedChatPrompt::PreparedChatPrompt() = default;
PreparedChatPrompt::~PreparedChatPrompt() = default;
PreparedChatPrompt::PreparedChatPrompt(PreparedChatPrompt&&) noexcept = default;
PreparedChatPrompt& PreparedChatPrompt::operator=(PreparedChatPrompt&&) noexcept = default;

PreparedChatPrompt PrepareTextChatPrompt(const chat_internal::PreparedChatMessages& messages,
                                         GenAIModelInstance& model,
                                         const ToolCallContext& tool_ctx) {
  if (messages.Empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  PreparedChatPrompt prepared;
  if (tool_ctx.harmony || (tool_ctx.tool_plan && !tool_ctx.HasForcedRawEnvelope())) {
    const auto original = BuildChatMessagesJson(messages.Messages(), tool_ctx.supports_reasoning_history);
    std::string prefix;
    do {
      prefix = "__foundry_literal_" + GenerateToolCallId() + "_";
    } while (original.find(prefix) != std::string::npos || tool_ctx.tools_json.find(prefix) != std::string::npos ||
             tool_ctx.template_kwargs_json.find(prefix) != std::string::npos);

    std::unordered_map<std::string, std::string> literals;
    const bool native_literals =
        !tool_ctx.harmony && (tool_ctx.native_qwen_xml_tool_calls || tool_ctx.native_tagged_json_tool_calls);
    const std::string serialization_witness = "\\\"\n@";
    const auto json_string_contents = [](const std::string& value) {
      auto encoded = nlohmann::json(value).dump();
      return encoded.substr(1, encoded.size() - 2);
    };
    const auto escaped_witness = json_string_contents(serialization_witness);
    const auto protect_literal = [&](std::string text) {
      if (text.empty()) {
        return text;
      }
      auto marker = prefix + std::to_string(literals.size()) + "__";
      literals.emplace(marker, std::move(text));
      // The template, not the output dialect, determines whether a fragment is JSON-escaped.
      return native_literals ? marker + serialization_witness : marker;
    };
    std::string projected;
    std::string projected_tools;
    if (tool_ctx.harmony) {
      auto native = BuildHarmonyPromptProjection(messages.Messages(), tool_ctx, protect_literal);
      projected = std::move(native.messages_json);
      projected_tools = std::move(native.tools_json);
    } else if (tool_ctx.native_qwen_xml_tool_calls || tool_ctx.native_tagged_json_tool_calls) {
      auto projected_json =
          nlohmann::ordered_json::parse(BuildChatMessagesJson(messages.Messages(),
                                                              tool_ctx.supports_reasoning_history));
      std::string state =
          "Use a tool only when the user's request requires it. For serial work, emit only calls whose "
          "prerequisites are already available; do not wait or deliberate about later calls.";
      if (tool_ctx.forced_tool) {
        state += "\nYou must call the tool named \"" + tool_ctx.forced_tool->name +
                 "\" in this turn. Do not answer with ordinary text.";
      } else if (!tool_ctx.text_output) {
        state += "\nYou must call one applicable supplied tool in this turn. Do not answer with ordinary text.";
      }
      if (!tool_ctx.automatic_retry_instruction.empty()) {
        state += "\n" + tool_ctx.automatic_retry_instruction;
      }
      if (!projected_json.empty() && projected_json.front().value("role", "") == "system") {
        auto content = projected_json.front().value("content", "");
        projected_json.front()["content"] = content.empty() ? state : content + "\n\n" + state;
      } else {
        projected_json.insert(projected_json.begin(),
                              nlohmann::ordered_json{{"role", "system"}, {"content", state}});
      }
      std::function<void(nlohmann::ordered_json&, bool)> protect_json_strings =
          [&](nlohmann::ordered_json& value, bool argument_data) {
            if (value.is_string()) {
              value = protect_literal(value.get<std::string>());
            } else if (value.is_array()) {
              for (auto& item : value) {
                protect_json_strings(item, argument_data);
              }
            } else if (value.is_object()) {
              auto protected_object = nlohmann::ordered_json::object();
              for (auto& [key, item] : value.items()) {
                if (argument_data || key != "type" || !item.is_string()) {
                  protect_json_strings(item, argument_data);
                }
                protected_object[argument_data ? protect_literal(key) : key] = std::move(item);
              }
              value = std::move(protected_object);
            }
          };
      for (auto& message : projected_json) {
        for (const auto* field : {"content", "reasoning_content"}) {
          if (message.contains(field)) {
            protect_json_strings(message[field], false);
          }
        }
        if (message.contains("tool_calls")) {
          for (auto& call : message["tool_calls"]) {
            if (!call.contains("function")) {
              continue;
            }
            auto& function = call["function"];
            if (function.contains("name")) {
              protect_json_strings(function["name"], false);
            }
            if (function.contains("arguments")) {
              protect_json_strings(function["arguments"], true);
            }
          }
        }
      }
      auto tools = nlohmann::ordered_json::parse(tool_ctx.tools_json);
      protect_json_strings(tools, false);
      projected = projected_json.dump();
      projected_tools = tools.dump();
    } else {
      projected = BuildCanonicalToolMessagesJson(messages.Messages(), tool_ctx, protect_literal);
    }
    auto& preprocessor = model.GetPreprocessor();
    const auto* kwargs = tool_ctx.template_kwargs_json.empty() ? nullptr : tool_ctx.template_kwargs_json.c_str();
    const auto* tools = projected_tools.empty() ? nullptr : projected_tools.c_str();
    const auto rendered = preprocessor.ApplyChatTemplateWithOptions(projected.c_str(), tools, kwargs, true);
    const auto append = [&](const std::string& text, bool literal) {
      if (text.empty()) {
        return;
      }
      auto sequences = literal ? preprocessor.EncodeLiteral(text.c_str()) : preprocessor.Encode(text.c_str());
      const auto* data = sequences->SequenceData(0);
      prepared.token_ids.insert(prepared.token_ids.end(), data, data + sequences->SequenceCount(0));
      prepared.prompt += text;
    };
    size_t cursor = 0;
    for (;;) {
      const auto next = rendered.find(prefix, cursor);
      if (next == std::string::npos) {
        append(rendered.substr(cursor), false);
        break;
      }

      const auto end = rendered.find("__", next + prefix.size());
      if (end == std::string::npos) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "chat template truncated a literal prompt fragment");
      }
      const auto found = literals.find(rendered.substr(next, end + 2 - next));
      if (found == literals.end()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "chat template changed a literal prompt fragment");
      }

      append(rendered.substr(cursor, next - cursor), false);
      cursor = end + 2;
      if (native_literals) {
        if (rendered.compare(cursor, escaped_witness.size(), escaped_witness) == 0) {
          append(json_string_contents(found->second), true);
          cursor += escaped_witness.size();
        } else if (rendered.compare(cursor, serialization_witness.size(), serialization_witness) == 0) {
          append(found->second, true);
          cursor += serialization_witness.size();
        } else {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                   "chat template changed a protected fragment's serialization");
        }
      } else {
        append(found->second, true);
      }
    }

    prepared.prompt_token_count = static_cast<int64_t>(prepared.token_ids.size());
    if (tool_ctx.harmony) {
      const auto& prefix = tool_ctx.harmony->assistant_prefix_tokens;
      if (!rendered.ends_with(HarmonyProtocol::kAssistantPrefix) || prepared.token_ids.size() < prefix.size() ||
          !std::equal(prefix.rbegin(), prefix.rend(), prepared.token_ids.rbegin())) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Harmony template produced an unqualified generation continuation");
      }
      prepared.harmony_continuation = HarmonyContinuation{
          "assistant", prefix.size(), std::char_traits<char>::length(HarmonyProtocol::kAssistantPrefix)};
    }
    return prepared;
  }

  prepared.prompt = BuildChatPrompt(messages, model, tool_ctx);
  auto sequences = EncodePrompt(prepared.prompt, model);
  const auto count = sequences->SequenceCount(0);
  const auto* data = sequences->SequenceData(0);
  prepared.token_ids.assign(data, data + count);
  prepared.prompt_token_count = static_cast<int64_t>(count);
  return prepared;
}

PreparedChatPrompt PrepareMediaChatPrompt(const std::vector<MessageItem>& messages,
                                          GenAIModelInstance& model,
                                          const std::vector<const ImageItem*>& images,
                                          const std::vector<const AudioItem*>& audios,
                                          const ToolCallContext& tool_ctx) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }
  if (images.empty() && audios.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "media preparation requires image or audio input");
  }
  if (!model.IsMultiModal()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "image or audio input requires a multimodal model");
  }
  if (!model.GetPreprocessor().HasMultiModalProcessor()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model has no multimodal processor available for media input");
  }

  PreparedChatPrompt prepared;
  const auto messages_json = OnnxChatGenerator::TransformMessagesForMedia(messages);
  const char* tools = tool_ctx.tools_json.empty() ? nullptr : tool_ctx.tools_json.c_str();
  const char* kwargs = tool_ctx.template_kwargs_json.empty() ? nullptr : tool_ctx.template_kwargs_json.c_str();
  prepared.prompt =
      model.GetPreprocessor().ApplyChatTemplateWithOptions(messages_json.c_str(), tools, kwargs, true);

  std::unique_ptr<OgaImages> oga_images;
  std::vector<std::vector<uint8_t>> image_bytes;
  if (!images.empty()) {
    image_bytes.reserve(images.size());
    std::vector<const void*> buffers;
    std::vector<size_t> sizes;
    buffers.reserve(images.size());
    sizes.reserve(images.size());

    for (const auto* image : images) {
      if (image == nullptr) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "image entry must not be null");
      }

      image_bytes.push_back(image->ReadBytes());
      buffers.push_back(image_bytes.back().data());
      sizes.push_back(image_bytes.back().size());
    }

    oga_images = OgaImages::Load(buffers.data(), sizes.data(), buffers.size());
  }

  std::unique_ptr<OgaAudios> oga_audios;
  if (!audios.empty()) {
    std::vector<const void*> buffers;
    std::vector<size_t> sizes;
    buffers.reserve(audios.size());
    sizes.reserve(audios.size());

    for (const auto* audio : audios) {
      if (audio == nullptr || audio->data == nullptr || audio->data_size == 0) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "audio entry must contain bytes");
      }

      buffers.push_back(audio->data);
      sizes.push_back(audio->data_size);
    }

    oga_audios = OgaAudios::Load(buffers.data(), sizes.data(), buffers.size());
  }

  prepared.media_tensors =
      model.GetPreprocessor().ProcessMedia(prepared.prompt.c_str(), oga_images.get(), oga_audios.get());
  const auto input_ids = prepared.media_tensors->Get("input_ids");
  const auto shape = input_ids->Shape();
  if (shape.empty() || shape.back() <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "multimodal processor returned invalid input_ids");
  }

  prepared.prompt_token_count = shape.back();
  return prepared;
}

}  // namespace fl
