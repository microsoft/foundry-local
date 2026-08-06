// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "exception.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "utils.h"

#include <nlohmann/json.hpp>
#include <ort_genai.h>

#include <algorithm>

namespace fl {

OnnxChatGenerator::~OnnxChatGenerator() = default;

// ---------------------------------------------------------------------------
// Private constructor
// ---------------------------------------------------------------------------

OnnxChatGenerator::OnnxChatGenerator(std::unique_ptr<OgaGeneratorParams> gen_params,
                                     std::unique_ptr<OgaGenerator> generator,
                                     std::unique_ptr<OgaTokenizerStream> stream,
                                     std::unique_ptr<OgaTokenizerStream> stream_with_special,
                                     GenAIModelInstance& model,
                                     int prompt_token_count,
                                     std::unique_ptr<OgaNamedTensors> named_tensors)
    : gen_params_(std::move(gen_params)),
      generator_(std::move(generator)),
      stream_(std::move(stream)),
      stream_with_special_(std::move(stream_with_special)),
      named_tensors_(std::move(named_tensors)),
      model_(model),
      prompt_token_count_(prompt_token_count) {}

// ---------------------------------------------------------------------------
// ChatGenerator interface
// ---------------------------------------------------------------------------

bool OnnxChatGenerator::IsDone() const {
  if (cancelled_) {
    return true;
  }

  // OgaGenerator::IsDone() is non-const in the ORT GenAI API, so we need const_cast.
  // This is safe because IsDone only reads state.
  auto* gen = const_cast<OgaGenerator*>(generator_.get());
  return gen->IsDone() || gen->IsSessionTerminated();
}

void OnnxChatGenerator::GenerateNextToken() {
  if (cancelled_) {
    return;
  }

  try {
    generator_->GenerateNextToken();
  } catch (const std::runtime_error& e) {
    // If cancelled while generating, the OGA engine throws when the session is terminated.
    // This is expected — not an error.
    if (cancelled_) {
      return;
    }

    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("token generation failed: ") + e.what());
  }
}

std::string OnnxChatGenerator::Decode() {
  if (cancelled_) {
    return "";
  }

  // Get the most recently generated token ID.
  // GetNextTokens returns the batch of next tokens; we use index 0 (batch size = 1).
  auto next_tokens = generator_->GetNextTokens();

  if (next_tokens.empty()) {
    return "";
  }

  int32_t token_id = next_tokens[0];

  // Decode through the normal tokenizer stream
  const char* token_text = stream_->Decode(token_id);

  // Also decode through the special-token stream to detect tool call and think tokens.
  // If the special stream gives a different result and it's a known special token type
  // that isn't an EOS token, surface the special representation instead.
  // Matches C# OnnxChatGenerator.Decode behavior.
  const char* special_text = stream_with_special_->Decode(token_id);

  std::string token_str = token_text ? std::string(token_text) : "";

  if (special_text != nullptr && token_text != nullptr && std::string(special_text) != token_str) {
    std::string special_str(special_text);
    bool is_tool_call_token = special_str.find("tool_call") != std::string::npos;
    bool is_think_token = special_str.find("think") != std::string::npos;

    const auto& eos_ids = model_.GetEosTokenIds();
    bool is_eos = std::find(eos_ids.begin(), eos_ids.end(), token_id) != eos_ids.end();

    if (!is_eos && (is_tool_call_token || is_think_token)) {
      return special_str;
    }
  }

  return token_str;
}

int OnnxChatGenerator::TokenCount() const {
  return static_cast<int>(generator_->GetSequenceCount(0));
}

int OnnxChatGenerator::PromptTokenCount() const {
  return prompt_token_count_;
}

void OnnxChatGenerator::Cancel() {
  cancelled_ = true;

  // Use the ORT GenAI engine-level termination to interrupt mid-compute
  // (e.g. during a long prefill), not just between token boundaries.
  try {
    generator_->SetRuntimeOption("terminate_session", "1");
  } catch (...) {
    // SetRuntimeOption may not be supported by all ORT GenAI builds
  }
}

// ---------------------------------------------------------------------------
// Continuous decoding: append new messages / rewind
// ---------------------------------------------------------------------------

int OnnxChatGenerator::AppendMessages(const std::vector<MessageItem>& new_messages,
                                      GenAIModelInstance& model,
                                      const std::string& tools_json) {
  if (new_messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "new_messages must not be empty");
  }

  // Build prompt from only the new messages. ApplyChatTemplate with add_generation_prompt=true
  // produces the correct continuation tokens (e.g. <|im_end|>\n<|im_start|>user\n...<|im_end|>\n<|im_start|>assistant\n)
  std::string prompt = BuildChatPrompt(new_messages, model, tools_json);
  auto sequences = EncodePrompt(prompt, model);
  int new_token_count = static_cast<int>(sequences->SequenceCount(0));

  try {
    generator_->AppendTokenSequences(*sequences);
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to append token sequences: ") + e.what());
  }

  return new_token_count;
}

void OnnxChatGenerator::RewindTo(int token_count) {
  try {
    generator_->RewindTo(token_count);
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to rewind generator: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Vision helpers (mirror upstream C# OnnxChatGenerator)
// ---------------------------------------------------------------------------

std::string OnnxChatGenerator::TransformMessagesForVision(const std::vector<MessageItem>& messages) {
  // Find the index of the last user message so we can rewrite only that one
  // into the structured `[{"type":"image"},{"type":"text","text":...}]` form.
  // Other messages are emitted in plain `{"role","content"}` form so the chat
  // template renders them the same way it does for text-only requests.
  size_t last_user_idx = messages.size();
  for (size_t i = messages.size(); i-- > 0;) {
    if (messages[i].role == FOUNDRY_LOCAL_ROLE_USER) {
      last_user_idx = i;
      break;
    }
  }

  nlohmann::json arr = nlohmann::json::array();
  for (size_t i = 0; i < messages.size(); ++i) {
    const auto& msg = messages[i];
    nlohmann::json entry;
    entry["role"] = Utils::RoleToString(msg.role);

    if (i == last_user_idx) {
      // Concatenate the message's text parts (REASONING parts and non-text parts skipped by the helper); image
      // bytes are processed separately by OgaMultiModalProcessor::ProcessImages.
      entry["content"] = nlohmann::json::array({
          nlohmann::json{{"type", "image"}},
          nlohmann::json{{"type", "text"}, {"text", RenderMessageForPrompt(msg)}},
      });
    } else {
      // Non-final message: render visible text parts only via the canonical helper.
      entry["content"] = RenderMessageForPrompt(msg);
    }

    arr.push_back(std::move(entry));
  }

  return arr.dump();
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::Create(const std::vector<MessageItem>& messages,
                                                             const SearchOptions& options,
                                                             GenAIModelInstance& model,
                                                             const ToolCallContext& tool_ctx,
                                                             bool use_full_context) {
  return CreateImpl(messages, options, model, tool_ctx, use_full_context, /*images=*/{});
}

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::CreateWithImages(
    const std::vector<MessageItem>& messages,
    const SearchOptions& options,
    GenAIModelInstance& model,
    const std::vector<const ImageItem*>& images,
    const ToolCallContext& tool_ctx,
    bool use_full_context) {
  if (images.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "CreateWithImages requires at least one image; use Create for text-only requests");
  }

  return CreateImpl(messages, options, model, tool_ctx, use_full_context, images);
}

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::CreateImpl(const std::vector<MessageItem>& messages,
                                                                 const SearchOptions& options,
                                                                 GenAIModelInstance& model,
                                                                 const ToolCallContext& tool_ctx,
                                                                 bool use_full_context,
                                                                 const std::vector<const ImageItem*>& images) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  const bool vision_branch = !images.empty();

  if (vision_branch) {
    if (!model.IsMultiModal()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "image input requires a multimodal model");
    }

    if (model.GetProcessor() == nullptr) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "model has no multimodal processor available for image input");
    }

    // Match upstream's single-image limit. Easy to relax once the wider
    // pipeline (and ORT GenAI templates) reliably handle multi-image inputs.
    if (images.size() > 1) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "only one image per request is supported");
    }
  }

  // 1. Build the chat prompt using the model's template.
  //    Vision: rewrite the last user message to insert the model's image
  //    sentinel before the user text.
  std::string prompt;
  if (vision_branch) {
    std::string messages_json = TransformMessagesForVision(messages);
    const char* tools_ptr = tool_ctx.tools_json.empty() ? nullptr : tool_ctx.tools_json.c_str();
    prompt = model.Tokenizer().ApplyChatTemplate(messages_json.c_str(), tools_ptr, /*add_generation_prompt=*/true);
  } else {
    prompt = BuildChatPrompt(messages, model, tool_ctx.tools_json);
  }

  // 2. Token budgeting.
  //    Text path: encode the prompt up front so we know its token count.
  //    Vision path: defer; OgaGenerator::SetInputs will derive input_ids from
  //    the named tensors, and we read TokenCount() back after the call.
  std::unique_ptr<OgaSequences> sequences;
  int input_token_count = 0;

  if (!vision_branch) {
    sequences = EncodePrompt(prompt, model);
    input_token_count = static_cast<int>(sequences->SequenceCount(0));
  } else {
    // Approximate budget for ApplySearchOptions: encode the prompt once with
    // the text tokenizer to get a token count. The actual prompt fed to the
    // generator comes from ProcessImages and may be slightly longer due to
    // image-token expansion, but this is the best estimate we have for
    // max_length budgeting and matches upstream's pattern of reading
    // TokenCount() after SetInputs for the authoritative count.
    auto approx = EncodePrompt(prompt, model);
    input_token_count = static_cast<int>(approx->SequenceCount(0));
  }

  // 3. Create GeneratorParams from the model
  auto gen_params = OgaGeneratorParams::Create(model.GetOgaModel());

  // 4. Apply search options (temperature, top_p, max_length, etc.) and validate token budget.
  //    Default output budget mirrors C# OnnxChatGenerator: 3072 for vision requests
  //    (image tokens push the prompt much higher), 2048 for text.
  int default_max_output = vision_branch ? 3072 : 2048;
  ApplySearchOptions(options, input_token_count, model.GetGenAIConfig(), *gen_params, model.EP(),
                     use_full_context, default_max_output);

  // 5. Compute guidance for constrained decoding.
  // Priority: user-specified guidance (from response_format) > auto-generated LARK grammar.
  // Matches C# GetGuidance() — always compute, then guard application.
  std::string guidance_type;
  std::string guidance_data;

  if (!tool_ctx.guidance_type.empty() && !tool_ctx.guidance_data.empty()) {
    // User specified guidance via response_format
    guidance_type = tool_ctx.guidance_type;
    guidance_data = tool_ctx.guidance_data;
  } else {
    // Auto-generate LARK grammar from tool definitions and reasoning state
    std::string json_schema;
    if (tool_ctx.HasTools()) {
      json_schema = BuildToolJsonSchema(tool_ctx);
    }

    guidance_data = BuildLarkGrammar(tool_ctx, json_schema);
    if (!guidance_data.empty()) {
      guidance_type = "lark_grammar";
    }
  }

  // Guard: Apply guidance only for tool-call-only mode (tool output requested, no text output). Text-only reasoning
  // (cot_text_only) cannot use grammar guidance because a completed grammar signals EOS to the ORT GenAI generator —
  // making IsDone() return true immediately on the next turn, breaking multi-turn continuous decoding. For
  // tool-call-only mode the generator is typically invalidated after a successful call anyway, so this is acceptable.
  // Reasoning content for text-only mode is handled via StripReasoningContent post-processing.
  bool tool_call_only = tool_ctx.tool_output && !tool_ctx.text_output;

  if (!guidance_type.empty() && !guidance_data.empty() && tool_call_only) {
    try {
      gen_params->SetGuidance(guidance_type.c_str(), guidance_data.c_str());
    } catch (const std::runtime_error& e) {
      // SetGuidance may not be supported by all models; continue without guidance
      (void)e;
    }
  }

  // 6. Create the Generator and feed it the prompt.
  //    Text path: append the encoded token sequences.
  //    Vision path: process images via OgaMultiModalProcessor and feed the
  //    resulting named tensors via SetInputs (which extracts input_ids and
  //    appends them internally — do NOT also call AppendTokenSequences).
  std::unique_ptr<OgaGenerator> generator;
  std::unique_ptr<OgaNamedTensors> named_tensors;
  try {
    generator = OgaGenerator::Create(model.GetOgaModel(), *gen_params);

    if (vision_branch) {
      // Materialise raw image bytes and pointer/size arrays for OgaImages::Load.
      std::vector<std::vector<std::uint8_t>> image_bytes;
      image_bytes.reserve(images.size());
      std::vector<const void*> buffers;
      buffers.reserve(images.size());
      std::vector<size_t> sizes;
      sizes.reserve(images.size());

      for (const auto* img : images) {
        if (img == nullptr) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "image entry must not be null");
        }

        image_bytes.push_back(img->ReadBytes());
        buffers.push_back(image_bytes.back().data());
        sizes.push_back(image_bytes.back().size());
      }

      auto oga_images = OgaImages::Load(buffers.data(), sizes.data(), buffers.size());
      named_tensors = model.GetProcessor()->ProcessImages(prompt.c_str(), oga_images.get());
      generator->SetInputs(*named_tensors);

      // Authoritative prompt token count after image-token expansion.
      input_token_count = static_cast<int>(generator->GetSequenceCount(0));
    } else {
      generator->AppendTokenSequences(*sequences);
    }
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to create generator: ") + e.what());
  }

  // 7. Create two tokenizer streams:
  //    - Normal stream: standard decoding (special tokens filtered)
  //    - Special stream: includes special tokens (for tool call detection)

  auto stream = OgaTokenizerStream::Create(model.Tokenizer().Oga());
  auto stream_with_special = OgaTokenizerStream::Create(model.GetOgaTokenizerWithSpecial());

  // `std::make_unique` constructs inside the library helper, which does not have
  // access to this class's private constructor.
  return std::unique_ptr<OnnxChatGenerator>(new OnnxChatGenerator(std::move(gen_params),
                                                                  std::move(generator),
                                                                  std::move(stream),
                                                                  std::move(stream_with_special),
                                                                  model,
                                                                  input_token_count,
                                                                  std::move(named_tensors)));
}

}  // namespace fl
