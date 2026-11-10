// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/item_codec.h"

#include "exception.h"
#include "items/audio_item.h"
#include "items/bytes_item.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/speech_result_item.h"
#include "items/speech_segment_item.h"
#include "items/tensor_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "util/file_uri.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;

template <typename Enum>
Enum ReadEnum(const Json& value, int minimum, int maximum) {
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(maximum))) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host item has an invalid enum value");
  }

  const auto number = value.get<int64_t>();
  if (number < minimum || number > maximum) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host item has an invalid enum value");
  }

  return static_cast<Enum>(number);
}

void RequireArray(const Json& value) {
  if (!value.is_array()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host item collection must be an array");
  }
}

template <typename T>
Json Optional(const std::optional<T>& value) {
  return value ? Json(*value) : Json(nullptr);
}

template <typename T>
std::optional<T> ReadOptional(const Json& value) {
  return value.is_null() ? std::nullopt : std::optional<T>(value.get<T>());
}

Json CopyBytes(const void* data, size_t size) {
  if (size == 0) {
    return Json::binary({});
  }
  if (!data) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host item has null data with a nonzero length");
  }

  const auto* begin = static_cast<const uint8_t*>(data);
  return Json::binary(std::vector<uint8_t>(begin, begin + size));
}

std::vector<uint8_t> ReadBytes(const Json& value) {
  if (!value.is_binary()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host item bytes must be a binary payload");
  }

  const auto& bytes = value.get_binary();
  return {bytes.begin(), bytes.end()};
}

std::string ResolveUri(const std::string& uri, const ItemEncodingContext& context) {
  if (uri.empty() || (uri.find("://") != std::string::npos && !uri.starts_with("file://"))) {
    return uri;
  }

  auto path = std::filesystem::path(PathFromFileUri(uri));
  if (path.is_relative()) {
    if (!context.caller_working_directory.is_absolute()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host item requires an absolute caller working directory");
    }

    path = context.caller_working_directory / path;
  }

  // The receiving SDK decodes file URIs again; preserve literal percent escapes in the resolved filename.
  std::string resolved = "file://";
  for (const char c : path.lexically_normal().string()) {
    if (c == '%') {
      resolved += "%25";
    } else {
      resolved += c;
    }
  }

  return resolved;
}

size_t TensorBytes(flTensorDataType type, const std::vector<int64_t>& shape) {
  size_t bits = 0;
  switch (type) {
    case FOUNDRY_LOCAL_TENSOR_UINT4:
    case FOUNDRY_LOCAL_TENSOR_INT4:
    case FOUNDRY_LOCAL_TENSOR_FLOAT4E2M1:
      bits = 4;
      break;
    case FOUNDRY_LOCAL_TENSOR_UINT8:
    case FOUNDRY_LOCAL_TENSOR_INT8:
    case FOUNDRY_LOCAL_TENSOR_BOOL:
    case FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FN:
    case FOUNDRY_LOCAL_TENSOR_FLOAT8E4M3FNUZ:
    case FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2:
    case FOUNDRY_LOCAL_TENSOR_FLOAT8E5M2FNUZ:
    case FOUNDRY_LOCAL_TENSOR_FLOAT8E8M0:
      bits = 8;
      break;
    case FOUNDRY_LOCAL_TENSOR_UINT16:
    case FOUNDRY_LOCAL_TENSOR_INT16:
    case FOUNDRY_LOCAL_TENSOR_FLOAT16:
    case FOUNDRY_LOCAL_TENSOR_BFLOAT16:
      bits = 16;
      break;
    case FOUNDRY_LOCAL_TENSOR_FLOAT:
    case FOUNDRY_LOCAL_TENSOR_INT32:
    case FOUNDRY_LOCAL_TENSOR_UINT32:
      bits = 32;
      break;
    case FOUNDRY_LOCAL_TENSOR_INT64:
    case FOUNDRY_LOCAL_TENSOR_DOUBLE:
    case FOUNDRY_LOCAL_TENSOR_UINT64:
    case FOUNDRY_LOCAL_TENSOR_COMPLEX64:
      bits = 64;
      break;
    case FOUNDRY_LOCAL_TENSOR_COMPLEX128:
      bits = 128;
      break;
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tensor has no fixed-width binary representation");
  }

  size_t elements = 1;
  for (const auto dimension : shape) {
    if (dimension < 0 || static_cast<uint64_t>(dimension) > std::numeric_limits<size_t>::max()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host tensor has an invalid dimension");
    }
    if (dimension != 0 && elements > std::numeric_limits<size_t>::max() / static_cast<size_t>(dimension)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host tensor shape overflows");
    }

    elements *= static_cast<size_t>(dimension);
  }

  if (elements > (std::numeric_limits<size_t>::max() - 7) / bits) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host tensor size overflows");
  }

  return (elements * bits + 7) / 8;
}

class OwnedBytes final : public BytesItem {
 public:
  OwnedBytes(flItemType item_type, std::vector<uint8_t> bytes) : BytesItem(item_type), bytes_(std::move(bytes)) {
    data = bytes_.empty() ? nullptr : bytes_.data();
    data_size = bytes_.size();
  }

 private:
  std::vector<uint8_t> bytes_;
};

class OwnedTensor final : public TensorItem {
 public:
  OwnedTensor(flTensorDataType type, std::vector<int64_t> shape, std::vector<uint8_t> bytes)
      : TensorItem(type, nullptr, std::move(shape)), bytes_(std::move(bytes)) {
    if (TensorBytes(data_type, this->shape) != bytes_.size()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host tensor shape and payload size do not match");
    }

    data = bytes_.empty() ? nullptr : bytes_.data();
  }

 private:
  std::vector<uint8_t> bytes_;
};

Json EncodeSegment(const SpeechSegmentItem& segment) {
  Json words = Json::array();
  for (const auto& word : segment.words) {
    words.push_back({{"text", word.text},
                     {"start", Optional(word.start_time_ms)},
                     {"end", Optional(word.end_time_ms)},
                     {"confidence", Optional(word.confidence)},
                     {"speaker", word.speaker_id}});
  }

  return {{"kind", segment.kind},
          {"text", segment.text},
          {"start", Optional(segment.start_time_ms)},
          {"end", Optional(segment.end_time_ms)},
          {"utterance_start", segment.utterance_start},
          {"words", std::move(words)},
          {"language", segment.language}};
}

std::unique_ptr<SpeechSegmentItem> DecodeSegment(const Json& value) {
  auto segment = std::make_unique<SpeechSegmentItem>();
  segment->kind = ReadEnum<flSpeechSegmentKind>(value.at("kind"), FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE,
                                                FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL);
  segment->text = value.at("text").get<std::string>();
  segment->start_time_ms = ReadOptional<int64_t>(value.at("start"));
  segment->end_time_ms = ReadOptional<int64_t>(value.at("end"));
  segment->utterance_start = value.at("utterance_start").get<bool>();
  segment->language = value.at("language").get<std::string>();
  const auto& words = value.at("words");
  RequireArray(words);
  for (const auto& encoded : words) {
    SpeechWord word;
    word.text = encoded.at("text").get<std::string>();
    word.start_time_ms = ReadOptional<int64_t>(encoded.at("start"));
    word.end_time_ms = ReadOptional<int64_t>(encoded.at("end"));
    word.confidence = ReadOptional<float>(encoded.at("confidence"));
    word.speaker_id = encoded.at("speaker").get<std::string>();
    segment->words.push_back(std::move(word));
  }

  segment->Finalize();
  return segment;
}

Json Encode(const Item& item, const ItemEncodingContext& context) {
  Json value{{"type", item.type}};
  switch (item.type) {
    case FOUNDRY_LOCAL_ITEM_TEXT: {
      const auto& text = static_cast<const TextItem&>(item);
      value["text"] = text.text;
      value["text_type"] = text.text_type;
      break;
    }
    case FOUNDRY_LOCAL_ITEM_MESSAGE: {
      const auto& message = static_cast<const MessageItem&>(item);
      value["role"] = message.role;
      value["name"] = message.name;
      value["parts"] = Json::array();
      for (const auto& part : message.content) {
        if (!part.view) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host message has a null content part");
        }

        value["parts"].push_back(Encode(*part.view, context));
      }

      break;
    }
    case FOUNDRY_LOCAL_ITEM_BYTES: {
      const auto& bytes = static_cast<const BytesItem&>(item);
      value["item_type"] = bytes.item_type;
      value["bytes"] = CopyBytes(bytes.data, bytes.data_size);
      break;
    }
    case FOUNDRY_LOCAL_ITEM_TENSOR: {
      const auto& tensor = static_cast<const TensorItem&>(item);
      value["data_type"] = tensor.data_type;
      value["shape"] = tensor.shape;
      value["bytes"] = CopyBytes(tensor.data, TensorBytes(tensor.data_type, tensor.shape));
      break;
    }
    case FOUNDRY_LOCAL_ITEM_IMAGE: {
      const auto& image = static_cast<const ImageItem&>(item);
      value["bytes"] = CopyBytes(image.data, image.data_size);
      value["format"] = image.format;
      value["uri"] = ResolveUri(image.uri, context);
      break;
    }
    case FOUNDRY_LOCAL_ITEM_AUDIO: {
      const auto& audio = static_cast<const AudioItem&>(item);
      value["bytes"] = CopyBytes(audio.data, audio.data_size);
      value["format"] = audio.format;
      value["uri"] = ResolveUri(audio.uri, context);
      value["sample_rate"] = audio.sample_rate;
      value["channels"] = audio.channels;
      break;
    }
    case FOUNDRY_LOCAL_ITEM_TOOL_CALL: {
      const auto& call = static_cast<const ToolCallItem&>(item);
      value["call_id"] = call.call_id;
      value["name"] = call.name;
      value["arguments"] = call.arguments;
      value["kind"] = call.kind;
      value["declared_kind"] = Optional(call.declared_kind);
      value["encoding"] = call.generated_encoding;
      value["replayed"] = call.replayed_from_store;
      value["replayed_arguments"] = call.replayed_arguments;
      value["replayed_kind"] = Optional(call.replayed_kind);
      break;
    }
    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT: {
      const auto& result = static_cast<const ToolResultItem&>(item);
      value["call_id"] = result.call_id;
      value["result"] = result.result;
      break;
    }
    case FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT:
      value["segment"] = EncodeSegment(static_cast<const SpeechSegmentItem&>(item));
      break;
    case FOUNDRY_LOCAL_ITEM_SPEECH_RESULT: {
      const auto& result = static_cast<const SpeechResultItem&>(item);
      value["text"] = result.text;
      value["language"] = result.language;
      value["duration"] = Optional(result.duration_ms);
      value["segments"] = Json::array();
      for (const auto& segment : result.segments) {
        value["segments"].push_back(segment ? EncodeSegment(*segment) : Json(nullptr));
      }

      break;
    }
    case FOUNDRY_LOCAL_ITEM_QUEUE: {
      if (!context.register_queue) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "shared-host item queue requires an invocation stream");
      }

      const auto id = context.register_queue(static_cast<const ItemQueue&>(item));
      if (id == 0) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host queue identity must not be zero");
      }

      value["queue"] = id;
      break;
    }
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown shared-host item type");
  }

  value["metadata"] = Json::object();
  if (const auto* metadata = item.GetMetadata()) {
    for (const auto& [key, entry] : metadata->Entries()) {
      value["metadata"][key] = entry;
    }
  }

  return value;
}

std::unique_ptr<Item> Decode(const Json& value, const ItemDecodingContext& context) {
  std::unique_ptr<Item> item;
  switch (ReadEnum<flItemType>(value.at("type"), 0, FOUNDRY_LOCAL_ITEM_QUEUE)) {
    case FOUNDRY_LOCAL_ITEM_TEXT:
      item = std::make_unique<TextItem>(
          value.at("text").get<std::string>(),
          ReadEnum<flTextItemType>(value.at("text_type"), 0, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
      break;
    case FOUNDRY_LOCAL_ITEM_MESSAGE: {
      std::vector<std::unique_ptr<Item>> parts;
      const auto& encoded_parts = value.at("parts");
      RequireArray(encoded_parts);
      for (const auto& encoded : encoded_parts) {
        const auto type = ReadEnum<flItemType>(encoded.at("type"), 0, FOUNDRY_LOCAL_ITEM_QUEUE);
        if (type != FOUNDRY_LOCAL_ITEM_TEXT && type != FOUNDRY_LOCAL_ITEM_IMAGE && type != FOUNDRY_LOCAL_ITEM_AUDIO) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host message content must be text, image or audio");
        }

        parts.push_back(Decode(encoded, context));
      }

      item = std::make_unique<MessageItem>(ReadEnum<flMessageRole>(value.at("role"), 0, FOUNDRY_LOCAL_ROLE_DEVELOPER),
                                           std::move(parts), value.at("name").get<std::string>());
      break;
    }
    case FOUNDRY_LOCAL_ITEM_BYTES:
      item = std::make_unique<OwnedBytes>(ReadEnum<flItemType>(value.at("item_type"), 0, FOUNDRY_LOCAL_ITEM_QUEUE),
                                          ReadBytes(value.at("bytes")));
      break;
    case FOUNDRY_LOCAL_ITEM_TENSOR:
      item = std::make_unique<OwnedTensor>(
          ReadEnum<flTensorDataType>(value.at("data_type"), 1, FOUNDRY_LOCAL_TENSOR_FLOAT8E8M0),
          value.at("shape").get<std::vector<int64_t>>(), ReadBytes(value.at("bytes")));
      break;
    case FOUNDRY_LOCAL_ITEM_IMAGE: {
      auto image = std::make_unique<ImageItem>(ReadBytes(value.at("bytes")), value.at("format").get<std::string>());
      image->uri = value.at("uri").get<std::string>();
      item = std::move(image);
      break;
    }
    case FOUNDRY_LOCAL_ITEM_AUDIO: {
      auto audio = std::make_unique<AudioItem>(ReadBytes(value.at("bytes")), value.at("format").get<std::string>());
      audio->uri = value.at("uri").get<std::string>();
      audio->sample_rate = value.at("sample_rate").get<int>();
      audio->channels = value.at("channels").get<int>();
      item = std::move(audio);
      break;
    }
    case FOUNDRY_LOCAL_ITEM_TOOL_CALL: {
      auto call =
          std::make_unique<ToolCallItem>(value.at("call_id").get<std::string>(), value.at("name").get<std::string>(),
                                         value.at("arguments").get<std::string>());
      call->kind = ReadEnum<ToolKind>(value.at("kind"), 0, static_cast<int>(ToolKind::kCustom));
      const auto& declared_kind = value.at("declared_kind");
      call->declared_kind =
          declared_kind.is_null()
              ? std::nullopt
              : std::optional(ReadEnum<ToolKind>(declared_kind, 0, static_cast<int>(ToolKind::kCustom)));
      call->generated_encoding = ReadEnum<GeneratedCallEncoding>(value.at("encoding"), 0,
                                                                 static_cast<int>(GeneratedCallEncoding::kRawEnvelope));
      call->replayed_from_store = value.at("replayed").get<bool>();
      call->replayed_arguments = value.at("replayed_arguments").get<std::string>();
      const auto& replayed_kind = value.at("replayed_kind");
      call->replayed_kind =
          replayed_kind.is_null()
              ? std::nullopt
              : std::optional(ReadEnum<ToolKind>(replayed_kind, 0, static_cast<int>(ToolKind::kCustom)));
      item = std::move(call);
      break;
    }
    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT:
      item = std::make_unique<ToolResultItem>(value.at("call_id").get<std::string>(),
                                              value.at("result").get<std::string>());
      break;
    case FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT:
      item = DecodeSegment(value.at("segment"));
      break;
    case FOUNDRY_LOCAL_ITEM_SPEECH_RESULT: {
      auto result = std::make_unique<SpeechResultItem>(value.at("text").get<std::string>());
      result->language = value.at("language").get<std::string>();
      result->duration_ms = ReadOptional<int64_t>(value.at("duration"));
      const auto& segments = value.at("segments");
      RequireArray(segments);
      for (const auto& segment : segments) {
        result->segments.push_back(segment.is_null() ? nullptr : DecodeSegment(segment));
      }

      result->Finalize();
      item = std::move(result);
      break;
    }
    case FOUNDRY_LOCAL_ITEM_QUEUE: {
      if (!context.open_queue || !value.at("queue").is_number_unsigned() || value.at("queue").get<uint64_t>() == 0) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host queue identity or invocation stream is invalid");
      }

      item = context.open_queue(value.at("queue").get<uint64_t>());
      if (!item) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host queue identity is not available");
      }

      break;
    }
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown shared-host item type");
  }

  const auto& metadata = value.at("metadata");
  if (!metadata.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host item metadata must be an object");
  }

  for (const auto& [key, entry] : metadata.items()) {
    item->GetMetadata().Add(key, entry.get<std::string>());
  }

  return item;
}

}  // namespace

nlohmann::json EncodeItem(const Item& item, const ItemEncodingContext& context) { return Encode(item, context); }

std::unique_ptr<Item> DecodeItem(const nlohmann::json& value, const ItemDecodingContext& context) {
  try {
    return Decode(value, context);
  } catch (const Json::exception&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid shared-host item payload");
  }
}

}  // namespace fl::hosting
