// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "exception.h"
#include "items/audio_item.h"
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
// Media helpers
// ---------------------------------------------------------------------------

std::string OnnxChatGenerator::TransformMessagesForMedia(const std::vector<MessageItem>& messages) {
  // Find the index of the last user message so we can rewrite only that one
  // into structured media markers followed by its text content.
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
      auto content = nlohmann::json::array();
      for (const auto& part : msg.content) {
        if (!part.view) {
          continue;
        }
        if (part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE) {
          content.push_back(nlohmann::json{{"type", "image"}});
        } else if (part.view->type == FOUNDRY_LOCAL_ITEM_AUDIO) {
          content.push_back(nlohmann::json{{"type", "audio"}});
        }
      }
      content.push_back(nlohmann::json{{"type", "text"}, {"text", RenderMessageForPrompt(msg)}});
      entry["content"] = std::move(content);
    } else {
      for (const auto& part : msg.content) {
        if (part.view &&
            (part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE || part.view->type == FOUNDRY_LOCAL_ITEM_AUDIO)) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                   "media input must belong to the final user message");
        }
      }
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
  return CreateImpl(messages, options, model, tool_ctx, use_full_context, /*images=*/{}, /*audios=*/{});
}

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::CreateWithMedia(
    const std::vector<MessageItem>& messages,
    const SearchOptions& options,
    GenAIModelInstance& model,
    const std::vector<const ImageItem*>& images,
    const std::vector<const AudioItem*>& audios,
    const ToolCallContext& tool_ctx,
    bool use_full_context) {
  if (images.empty() && audios.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "CreateWithMedia requires at least one image or audio input");
  }
  return CreateImpl(messages, options, model, tool_ctx, use_full_context, images, audios);
}

std::unique_ptr<OnnxChatGenerator> OnnxChatGenerator::CreateImpl(const std::vector<MessageItem>& messages,
                                                                 const SearchOptions& options,
                                                                 GenAIModelInstance& model,
                                                                 const ToolCallContext& tool_ctx,
                                                                 bool use_full_context,
                                                                 const std::vector<const ImageItem*>& images,
                                                                 const std::vector<const AudioItem*>& audios) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  const bool media_branch = !images.empty() || !audios.empty();

  if (media_branch) {
    if (!model.IsMultiModal()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "image or audio input requires a multimodal model");
    }

    if (model.GetProcessor() == nullptr) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "model has no multimodal processor available for media input");
    }

    // Match upstream's single-image limit. Easy to relax once the wider
    // pipeline (and ORT GenAI templates) reliably handle multi-image inputs.
    if (images.size() > 1) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "only one image per request is supported");
    }
  }

  // 1. Build the chat prompt using the model's template.
  //    Media: rewrite the last user message to insert media sentinels.
  std::string prompt;
  if (media_branch) {
    std::string messages_json = TransformMessagesForMedia(messages);
    const char* tools_ptr = tool_ctx.tools_json.empty() ? nullptr : tool_ctx.tools_json.c_str();
    prompt = model.Tokenizer().ApplyChatTemplate(messages_json.c_str(), tools_ptr, /*add_generation_prompt=*/true);
  } else {
    prompt = BuildChatPrompt(messages, model, tool_ctx.tools_json);
  }

  // 2. Token budgeting.
  //    Text path: encode the prompt up front so we know its token count.
  //    Media path: process inputs first and read the expanded input_ids shape.
  std::unique_ptr<OgaSequences> sequences;
  int input_token_count = 0;

  if (!media_branch) {
    sequences = EncodePrompt(prompt, model);
    input_token_count = static_cast<int>(sequences->SequenceCount(0));
  }

  // Process media before sizing the generator so max_length includes the
  // exact token expansion produced by the multimodal processor.
  std::unique_ptr<OgaNamedTensors> named_tensors;
  if (media_branch) {
    std::unique_ptr<OgaImages> oga_images;
    if (!images.empty()) {
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

      oga_images = OgaImages::Load(buffers.data(), sizes.data(), buffers.size());
    }

    std::unique_ptr<OgaAudios> oga_audios;
    if (!audios.empty()) {
      std::vector<const void*> audio_buffers;
      audio_buffers.reserve(audios.size());
      std::vector<size_t> audio_sizes;
      audio_sizes.reserve(audios.size());
      for (const auto* audio : audios) {
        if (audio == nullptr || audio->data == nullptr || audio->data_size == 0) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "audio entry must contain bytes");
        }
        audio_buffers.push_back(audio->data);
        audio_sizes.push_back(audio->data_size);
      }

      oga_audios = OgaAudios::Load(audio_buffers.data(), audio_sizes.data(), audio_buffers.size());
    }

    if (oga_images && oga_audios) {
      named_tensors = model.GetProcessor()->ProcessImagesAndAudios(prompt.c_str(), oga_images.get(), oga_audios.get());
    } else if (oga_images) {
      named_tensors = model.GetProcessor()->ProcessImages(prompt.c_str(), oga_images.get());
    } else {
      named_tensors = model.GetProcessor()->ProcessAudios(prompt.c_str(), oga_audios.get());
    }
    auto input_ids = named_tensors->Get("input_ids");
    auto input_shape = input_ids->Shape();
    if (input_shape.empty() || input_shape.back() <= 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "multimodal processor returned invalid input_ids");
    }
    input_token_count = static_cast<int>(input_shape.back());
  }

  // 3. Create GeneratorParams from the model
  auto gen_params = OgaGeneratorParams::Create(model.GetOgaModel());

  // 4. Apply search options (temperature, top_p, max_length, etc.) and validate token budget.
  //    Media inputs use a larger default because preprocessing expands them into tokens.
  int default_max_output = media_branch ? 3072 : 2048;
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
  //    Media path: process inputs via OgaMultiModalProcessor and feed the
  //    resulting named tensors via SetInputs (which extracts input_ids and
  //    appends them internally — do NOT also call AppendTokenSequences).
  std::unique_ptr<OgaGenerator> generator;
  try {
    generator = OgaGenerator::Create(model.GetOgaModel(), *gen_params);

    if (media_branch) {
      generator->SetInputs(*named_tensors);
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
