// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "exception.h"
#include "hosting/sdk_values.h"
#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/search_options.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"

#include <gtest/gtest.h>

#include <limits>

namespace {

using nlohmann::json;
using namespace fl::hosting;

json ThroughCbor(const json& value) { return json::from_cbor(json::to_cbor(value)); }

TEST(SdkValuesTest, CapturesReplayAndPreparedToolContextWithoutChangingPayloadText) {
  fl::Request request;
  request.options.Add("max_output_tokens", "17");
  request.AddOwnedItem(std::make_unique<fl::TextItem>("initial"));
  auto call = std::make_unique<fl::ToolCallItem>();
  call->call_id = "call-1";
  call->name = "apply_patch";
  call->arguments = "line 1\n  raw payload\n";
  request.AddOwnedItem(std::move(call));
  request.item_segment_starts = {0, 1, 1, 4};
  request.prepared_tool_definitions =
      std::vector<fl::ToolDefinition>{{"apply_patch", "description", "", fl::ToolKind::kCustom, false, false, false}};
  request.prepared_tool_definitions->front().custom_lark_grammar = "start: /.+/";
  request.forced_tool_choice = fl::ForcedToolChoice{"apply_patch", fl::ToolKind::kCustom};
  request.raw_envelope_descriptor = fl::RawEnvelopeDescriptor{"apply_patch", "<start>", "<end>"};

  const auto encoded = ThroughCbor(EncodeRequest(request, {}));
  auto decoded = DecodeRequest(encoded, {});
  request.items.clear();
  ASSERT_EQ(decoded.items.size(), 2);
  EXPECT_EQ(static_cast<const fl::ToolCallItem&>(*decoded.items.back()).arguments, "line 1\n  raw payload\n");
  EXPECT_STREQ(decoded.options.Find("max_output_tokens"), "17");
  EXPECT_EQ(decoded.item_segment_starts, (std::vector<size_t>{0, 1, 1, 4}));
  ASSERT_TRUE(decoded.prepared_tool_definitions);
  const auto& tool = decoded.prepared_tool_definitions->front();
  EXPECT_EQ(tool.kind, fl::ToolKind::kCustom);
  EXPECT_FALSE(tool.include_description_in_prompt);
  EXPECT_FALSE(tool.include_parameters_in_prompt);
  ASSERT_TRUE(tool.strict);
  EXPECT_FALSE(*tool.strict);
  EXPECT_EQ(tool.custom_lark_grammar, "start: /.+/");
  ASSERT_TRUE(decoded.forced_tool_choice);
  EXPECT_EQ(decoded.forced_tool_choice->name, "apply_patch");
  EXPECT_EQ(decoded.forced_tool_choice->kind, fl::ToolKind::kCustom);
  EXPECT_EQ(decoded.raw_envelope_descriptor, request.raw_envelope_descriptor);
}

TEST(SdkValuesTest, PreservesTypedExecutionMetadataAndIndependentExtensiblePropertyBags) {
  fl::ModelInfo info;
  info.model_id = "older-version:7";
  info.name = "older-version";
  info.version = 7;
  info.alias = "alias";
  info.uri = "model-uri";
  info.task = "coding";
  info.execution_provider = "CPUExecutionProvider";
  info.device_type = fl::DeviceType::kCPU;
  info.detected_region = "region";
  info.string_properties.emplace("custom", "unmodified\nmetadata");
  info.int_properties.emplace(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT, 1);
  info.int_properties.emplace("large-value", int64_t{1} << 45);
  info.prompt_templates.Add("user", "{{content}}");
  info.model_settings.Add("setting", "value");
  auto decoded = DecodeModelInfo(ThroughCbor(EncodeModelInfo(info)));
  info.string_properties.clear();
  EXPECT_EQ(decoded.model_id, "older-version:7");
  EXPECT_EQ(decoded.task, "coding");
  EXPECT_EQ(decoded.execution_provider, "CPUExecutionProvider");
  EXPECT_EQ(decoded.device_type, fl::DeviceType::kCPU);
  EXPECT_EQ(decoded.detected_region, "region");
  EXPECT_EQ(decoded.string_properties.at("custom"), "unmodified\nmetadata");
  EXPECT_EQ(decoded.int_properties.at("large-value"), int64_t{1} << 45);
  EXPECT_STREQ(decoded.prompt_templates.Find("user"), "{{content}}");
  EXPECT_STREQ(decoded.model_settings.Find("setting"), "value");
}

TEST(SdkValuesTest, RejectsInvalidModelDiscriminatorsAndLossyMetadataIntegers) {
  auto metadata = EncodeModelInfo(fl::ModelInfo{});
  metadata["device"] = uint64_t{1} << 32;
  EXPECT_THROW(DecodeModelInfo(metadata), fl::Exception);
  metadata["device"] = -1;
  EXPECT_THROW(DecodeModelInfo(metadata), fl::Exception);
  metadata["device"] = 0;
  metadata["version"] = uint64_t{1} << 32;
  EXPECT_THROW(DecodeModelInfo(metadata), fl::Exception);
  metadata["version"] = 0;
  metadata["integers"] = {{"value", std::numeric_limits<uint64_t>::max()}};
  EXPECT_THROW(DecodeModelInfo(metadata), fl::Exception);
  metadata["integers"] = {{"value", true}};
  EXPECT_THROW(DecodeModelInfo(metadata), fl::Exception);
  EXPECT_THROW(DecodeModelSelection({{"catalog", "unknown"}, {"info", metadata}, {"path", ""}}), fl::Exception);
}

TEST(SdkValuesTest, DoesNotTransmitReusableRequestCancellationState) {
  fl::Request request;
  ASSERT_TRUE(request.TryBegin());
  ASSERT_TRUE(request.Cancel());
  auto decoded = DecodeRequest(ThroughCbor(EncodeRequest(request, {})), {});
  EXPECT_FALSE(decoded.IsCancellationRequested());
  EXPECT_TRUE(decoded.TryBegin());
  EXPECT_FALSE(decoded.prepared_tool_definitions);

  auto empty_tools = EncodeRequest(request, {});
  empty_tools["prepared_tools"] = json::array();
  auto prepared = DecodeRequest(empty_tools, {});
  ASSERT_TRUE(prepared.prepared_tool_definitions);
  EXPECT_TRUE(prepared.prepared_tool_definitions->empty());
}

TEST(SdkValuesTest, MaterializesResponseItemsAndAllUsageAndTerminationFields) {
  fl::Response response;
  response.items.push_back(std::make_unique<fl::TextItem>("answer"));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_LENGTH;
  response.termination_cause = fl::BackendTerminationCause::kSessionTokenLimit;
  response.usage = {19, 7, 26, 3, 11};
  response.metadata.Add("completion_id", "private-id");
  response.openai_json_message_count = std::nullopt;
  auto decoded = DecodeResponse(ThroughCbor(EncodeResponse(response, {})), {});
  response.items.clear();
  ASSERT_EQ(decoded.items.size(), 1);
  EXPECT_EQ(static_cast<const fl::TextItem&>(*decoded.items.front()).text, "answer");
  EXPECT_EQ(decoded.finish_reason, FOUNDRY_LOCAL_FINISH_LENGTH);
  EXPECT_EQ(decoded.termination_cause, fl::BackendTerminationCause::kSessionTokenLimit);
  EXPECT_EQ(decoded.usage.prompt_tokens, 19);
  EXPECT_EQ(decoded.usage.completion_tokens, 7);
  EXPECT_EQ(decoded.usage.total_tokens, 26);
  EXPECT_EQ(decoded.usage.reasoning_tokens, 3);
  EXPECT_EQ(decoded.usage.cached_prompt_tokens, 11);
  EXPECT_STREQ(decoded.metadata.Find("completion_id"), "private-id");
  EXPECT_FALSE(decoded.openai_json_message_count);
}

TEST(SdkValuesTest, RejectsMalformedCollectionsAndLossyIntegerConversions) {
  fl::Request request;
  auto encoded_request = EncodeRequest(request, {});
  encoded_request["segments"] = {-1};
  EXPECT_THROW(DecodeRequest(encoded_request, {}), fl::Exception);
  encoded_request["segments"] = {2, 1};
  EXPECT_THROW(DecodeRequest(encoded_request, {}), fl::Exception);
  encoded_request["segments"] = json::object();
  EXPECT_THROW(DecodeRequest(encoded_request, {}), fl::Exception);
  encoded_request = EncodeRequest(request, {});
  encoded_request["options"] = {{"option", 7}};
  EXPECT_THROW(DecodeRequest(encoded_request, {}), fl::Exception);

  fl::Response response;
  auto encoded_response = EncodeResponse(response, {});
  encoded_response["usage"]["prompt"] = std::numeric_limits<uint64_t>::max();
  EXPECT_THROW(DecodeResponse(encoded_response, {}), fl::Exception);
  encoded_response = EncodeResponse(response, {});
  encoded_response["finish"] = uint64_t{1} << 32;
  EXPECT_THROW(DecodeResponse(encoded_response, {}), fl::Exception);
  encoded_response = EncodeResponse(response, {});
  encoded_response["json_message_count"] = -1;
  EXPECT_THROW(DecodeResponse(encoded_response, {}), fl::Exception);
  EXPECT_THROW(DecodeToolDefinition(json::object()), fl::Exception);
}

TEST(SdkValuesTest, PreservesExactPreflightIntegersWithoutNarrowing) {
  const fl::RequestBudget budget{int64_t{1} << 33, 17, (int64_t{1} << 33) + 17, int64_t{1} << 34, true, 0};
  const auto decoded = DecodeBudget(ThroughCbor(EncodeBudget(budget)));
  EXPECT_EQ(decoded.prompt_tokens, budget.prompt_tokens);
  EXPECT_EQ(decoded.required_tokens, budget.required_tokens);
  EXPECT_EQ(decoded.context_limit_tokens, budget.context_limit_tokens);
  EXPECT_TRUE(decoded.fits);
  auto invalid = EncodeBudget(budget);
  invalid["prompt"] = 1.5;
  EXPECT_THROW(DecodeBudget(invalid), fl::Exception);
}

}  // namespace
