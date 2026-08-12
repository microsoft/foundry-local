// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session.h"

#include <gtest/gtest.h>

using namespace fl;

TEST(SessionDispatchTest, TextGenerationTasksMapToChatSessionType) {
  EXPECT_EQ(ClassifySessionTask("text-generation"), SessionType::kChat);
  EXPECT_EQ(ClassifySessionTask("text2text-generation"), SessionType::kChat);
}

TEST(SessionDispatchTest, ExistingTasksKeepTheirSessionTypes) {
  EXPECT_EQ(ClassifySessionTask("chat-completion"), SessionType::kChat);
  EXPECT_EQ(ClassifySessionTask("vision-language-chat"), SessionType::kChat);
  EXPECT_EQ(ClassifySessionTask("automatic-speech-recognition"), SessionType::kAudio);
  EXPECT_EQ(ClassifySessionTask("embeddings"), SessionType::kEmbeddings);
}

TEST(SessionDispatchTest, UnsupportedTasksReturnNullopt) {
  EXPECT_FALSE(ClassifySessionTask("").has_value());
  EXPECT_FALSE(ClassifySessionTask("unsupported").has_value());
}
