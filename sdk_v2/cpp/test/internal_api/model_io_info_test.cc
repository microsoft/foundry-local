// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for Model::GetInputOutputInfo — verifies correct IO type descriptors
// are returned based on the model's task, and that unknown tasks throw.
//
#include "exception.h"
#include "internal_api/test_helpers.h"
#include "items/item.h"
#include "items/text_item.h"
#include "model.h"
#include "model_info.h"

#include <gtest/gtest.h>

using namespace fl;

// Helper: create a leaf Model with the given task string.
static Model MakeModelWithTask(const std::string& task) {
  static fl::test::FakeServiceBindings svc;
  ModelInfo info;
  info.model_id = "test-model";
  info.name = "test";
  info.version = 1;
  info.alias = "test-alias";
  info.task = task;
  return Model::FromModelInfo(std::move(info), "",
                              svc.download_manager, svc.model_load_manager);
}

// ========================================================================
// Chat-completion task
// ========================================================================

TEST(ModelIOInfoTest, ChatCompletion_ReturnsMessageAndOpenAIJsonInputs) {
  auto model = MakeModelWithTask("chat-completion");
  auto io = model.GetInputOutputInfo();

  ASSERT_EQ(io.num_inputs, 2u);
  EXPECT_EQ(io.inputs[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(io.inputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.inputs[1])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
}

TEST(ModelIOInfoTest, ChatCompletion_ReturnsMessageAndOpenAIJsonOutputs) {
  auto model = MakeModelWithTask("chat-completion");
  auto io = model.GetInputOutputInfo();

  ASSERT_EQ(io.num_outputs, 2u);
  EXPECT_EQ(io.outputs[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(io.outputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.outputs[1])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
}

// ========================================================================
// Vision-language-chat task
// ========================================================================

TEST(ModelIOInfoTest, VisionLanguageChat_ReturnsImageInput) {
  auto model = MakeModelWithTask("vision-language-chat");
  auto io = model.GetInputOutputInfo();

  ASSERT_EQ(io.num_inputs, 3u);
  EXPECT_EQ(io.inputs[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(io.inputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.inputs[1])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  EXPECT_EQ(io.inputs[2]->type, FOUNDRY_LOCAL_ITEM_IMAGE);
}

TEST(ModelIOInfoTest, VisionLanguageChat_ReturnsChatOutputs) {
  auto model = MakeModelWithTask("vision-language-chat");
  auto io = model.GetInputOutputInfo();

  ASSERT_EQ(io.num_outputs, 2u);
  EXPECT_EQ(io.outputs[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(io.outputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.outputs[1])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
}

// ========================================================================
// Automatic speech recognition task
// ========================================================================

TEST(ModelIOInfoTest, ASR_ReturnsAudioAndOpenAIJsonInputs) {
  auto model = MakeModelWithTask("automatic-speech-recognition");
  auto io = model.GetInputOutputInfo();

  ASSERT_EQ(io.num_inputs, 2u);
  EXPECT_EQ(io.inputs[0]->type, FOUNDRY_LOCAL_ITEM_AUDIO);
  EXPECT_EQ(io.inputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.inputs[1])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
}

TEST(ModelIOInfoTest, ASR_ReturnsTextAndOpenAIJsonOutputs) {
  auto model = MakeModelWithTask("automatic-speech-recognition");
  auto io = model.GetInputOutputInfo();

  ASSERT_EQ(io.num_outputs, 2u);
  EXPECT_EQ(io.outputs[0]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(io.outputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.outputs[0])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
  EXPECT_EQ(static_cast<const TextItem*>(io.outputs[1])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
}

// ========================================================================
// Unknown task — throws
// ========================================================================

TEST(ModelIOInfoTest, UnknownTask_Throws) {
  auto model = MakeModelWithTask("some-unsupported-task");

  try {
    model.GetInputOutputInfo();
    FAIL() << "Expected fl::Exception for unknown task";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED);
    EXPECT_NE(std::string(e.what()).find("some-unsupported-task"), std::string::npos)
        << "Error message should mention the task. Got: " << e.what();
  }
}

TEST(ModelIOInfoTest, EmptyTask_Throws) {
  auto model = MakeModelWithTask("");

  EXPECT_THROW(model.GetInputOutputInfo(), fl::Exception);
}

// ========================================================================
// Container delegation
// ========================================================================

TEST(ModelIOInfoTest, Container_DelegatesToSelectedVariant) {
  auto variant = MakeModelWithTask("chat-completion");
  auto container = Model::MakeContainer(std::move(variant));

  auto io = container.GetInputOutputInfo();

  // Should see the same chat-completion IO as a leaf.
  ASSERT_EQ(io.num_inputs, 2u);
  EXPECT_EQ(io.inputs[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(io.inputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.inputs[1])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  ASSERT_EQ(io.num_outputs, 2u);
  EXPECT_EQ(io.outputs[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(io.outputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.outputs[1])->text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
}

TEST(ModelIOInfoTest, Container_DelegatesToSelectedVariant_ASR) {
  auto chat_variant = MakeModelWithTask("chat-completion");
  auto asr_variant = MakeModelWithTask("automatic-speech-recognition");
  auto container = Model::MakeContainer(std::move(chat_variant));
  container.AddVariant(std::move(asr_variant));

  // Default selection is the first variant (chat-completion).
  auto io = container.GetInputOutputInfo();
  ASSERT_EQ(io.num_inputs, 2u);
  EXPECT_EQ(io.inputs[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
}

// ========================================================================
// Pointer stability — static caches return the same pointers
// ========================================================================

TEST(ModelIOInfoTest, StaticCache_ReturnsSamePointers) {
  auto model1 = MakeModelWithTask("chat-completion");
  auto model2 = MakeModelWithTask("chat-completion");

  auto io1 = model1.GetInputOutputInfo();
  auto io2 = model2.GetInputOutputInfo();

  // Pointers come from static storage, so they must be identical.
  EXPECT_EQ(io1.inputs, io2.inputs);
  EXPECT_EQ(io1.outputs, io2.outputs);
  EXPECT_EQ(io1.inputs[0], io2.inputs[0]);
  EXPECT_EQ(io1.outputs[0], io2.outputs[0]);
}

TEST(ModelIOInfoTest, StaticCache_DifferentTasks_DifferentPointers) {
  auto chat_model = MakeModelWithTask("chat-completion");
  auto asr_model = MakeModelWithTask("automatic-speech-recognition");

  auto chat_io = chat_model.GetInputOutputInfo();
  auto asr_io = asr_model.GetInputOutputInfo();

  // Different tasks must have different pointer arrays.
  EXPECT_NE(chat_io.inputs, asr_io.inputs);
  EXPECT_NE(chat_io.outputs, asr_io.outputs);
}
