// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Direct ChatSession integration tests for vision models.
//
// Unlike responses_vision_test.cc (which goes through the web service),
// these tests exercise ChatSession directly via the public C++ API with
// real image bytes loaded from testdata/Taittinger.jpg.

#include "model_fixture.h"

#include "utils/string_utils.h"

using fl::test::ToLower;

namespace {

fs::path GetImagePath() {
#ifdef FOUNDRY_LOCAL_TEST_DATA_DIR
  return fs::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "Taittinger.jpg";
#else
  return fs::current_path() / "testdata" / "Taittinger.jpg";
#endif
}

std::vector<uint8_t> LoadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

// ========================================================================
// Tests — use the shared VisionFixture from model_fixture.h
// ========================================================================

TEST_F(VisionFixture, ChatSessionWithImageProducesDescriptiveOutput) {
  using namespace foundry_local;

  auto image_path = GetImagePath();
  if (!fs::exists(image_path)) {
    GTEST_SKIP() << "testdata/Taittinger.jpg not found";
  }
  auto image_bytes = LoadFile(image_path);

  ChatSession session(vision_model());
  // Vision models expand image bytes into hundreds of visual tokens. The ORT
  // GenAI max_length is computed from text-approximate prompt + max_output_tokens,
  // so we set this high enough to cover the post-expansion token count.
  RequestOptions session_opts;
  session_opts.search.temperature = 0.0f;
  session_opts.search.max_output_tokens = 1024;
  session.SetOptions(session_opts);

  // Build a multi-part user message: text prompt + image bytes.
  std::vector<Item> parts;
  parts.push_back(Item::Text("Describe this image in one short sentence."));
  parts.push_back(Item::ImageFromData("jpeg", image_bytes.data(), image_bytes.size()));

  Request request{
      MessageItem(FOUNDRY_LOCAL_ROLE_USER, std::move(parts)),
  };

  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP)
      << "Response should have STOP finish reason";
  EXPECT_FALSE(response.GetItems().empty())
      << "Response should contain at least one output item";

  std::string output_text = CollectResponseText(response);
  std::string lower = ToLower(output_text);

  // The image shows bottles of Taittinger champagne.
  EXPECT_NE(lower.find("bottle"), std::string::npos)
      << "Expected vision model to mention bottles. Got: " << output_text;
  std::cout << "Vision output: " << output_text << "\n";

  auto usage = response.GetUsage();
  EXPECT_GT(usage.prompt_tokens, 0) << "Should report prompt token usage";
  EXPECT_GT(usage.completion_tokens, 0) << "Should report completion token usage";
}

TEST_F(VisionFixture, SessionBaseClassWithVisionModelWorks) {
  using namespace foundry_local;

  auto image_path = GetImagePath();
  if (!fs::exists(image_path)) {
    GTEST_SKIP() << "testdata/Taittinger.jpg not found";
  }
  auto image_bytes = LoadFile(image_path);

  // Verify that the base Session class also works with vision models (no task check in Session).
  Session session(vision_model());
  RequestOptions session_opts;
  session_opts.search.temperature = 0.0f;
  session_opts.search.max_output_tokens = 1024;
  session.SetOptions(session_opts);

  std::vector<Item> parts;
  parts.push_back(Item::Text("What is in this image? One word."));
  parts.push_back(Item::ImageFromData("jpeg", image_bytes.data(), image_bytes.size()));

  Request request{
      MessageItem(FOUNDRY_LOCAL_ROLE_USER, std::move(parts)),
  };

  Response response = session.ProcessRequest(request);

  std::string output_text = CollectResponseText(response);
  EXPECT_FALSE(output_text.empty()) << "Session base class should produce output for vision model";
  std::cout << "Session-base vision output: " << output_text << "\n";
}
