// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/genai_config.h"

#include "exception.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace fl;

namespace {

/// Create a temporary directory for test files.
class GenAIConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "genai_config_test";
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override {
    std::filesystem::remove_all(test_dir_);
  }

  /// Write content to a file in the test directory and return the path.
  std::string WriteFile(const std::string& filename, const std::string& content) {
    auto path = test_dir_ / filename;
    std::ofstream f(path);
    f << content;
    f.close();
    return path.string();
  }

  std::filesystem::path test_dir_;
};

}  // anonymous namespace

// ========================================================================
// IsMultiModal
// ========================================================================

TEST(GenAIConfigModelTest, IsMultiModalTrueForKnownTypes) {
  GenAIConfig::OnnxModel model;

  model.type = "phi3v";
  EXPECT_TRUE(model.IsMultiModal());

  model.type = "whisper";
  EXPECT_TRUE(model.IsMultiModal());

  model.type = "phi4mm";
  EXPECT_TRUE(model.IsMultiModal());

  model.type = "fara";
  EXPECT_TRUE(model.IsMultiModal());

  model.type = "qwen2_5_vl";
  EXPECT_TRUE(model.IsMultiModal());

  model.type = "qwen3_vl";
  EXPECT_TRUE(model.IsMultiModal());

  model.type = "qwen3_5";
  EXPECT_TRUE(model.IsMultiModal());

  model.type = "gemma4";
  EXPECT_TRUE(model.IsMultiModal());

  model.type = "mistral3";
  EXPECT_TRUE(model.IsMultiModal());
}

TEST(GenAIConfigModelTest, IsMultiModalFalseForOtherTypes) {
  GenAIConfig::OnnxModel model;

  model.type = "phi3";
  EXPECT_FALSE(model.IsMultiModal());

  model.type = "";
  EXPECT_FALSE(model.IsMultiModal());

  model.type = "gpt2";
  EXPECT_FALSE(model.IsMultiModal());
}

// ========================================================================
// DefaultProvider
// ========================================================================

TEST(GenAIConfigDefaultProviderTest, ReturnsFirstProviderKey) {
  GenAIConfig config;
  GenAIConfig::OnnxModel model;
  GenAIConfig::OnnxModel::Decoder decoder;
  GenAIConfig::OnnxModel::Decoder::SessionOptions opts;

  opts.provider_options.push_back({{"dml", "{}"}});
  opts.provider_options.push_back({{"cuda", "{}"}});

  decoder.session_options = std::move(opts);
  model.decoder = std::move(decoder);
  config.model = std::move(model);

  EXPECT_EQ(config.DefaultProvider(), "dml");
}

TEST(GenAIConfigDefaultProviderTest, EmptyWhenNoModel) {
  GenAIConfig config;
  EXPECT_EQ(config.DefaultProvider(), "");
}

TEST(GenAIConfigDefaultProviderTest, EmptyWhenNoDecoder) {
  GenAIConfig config;
  config.model = GenAIConfig::OnnxModel{};
  EXPECT_EQ(config.DefaultProvider(), "");
}

TEST(GenAIConfigDefaultProviderTest, EmptyWhenNoSessionOptions) {
  GenAIConfig config;
  GenAIConfig::OnnxModel model;
  model.decoder = GenAIConfig::OnnxModel::Decoder{};
  config.model = std::move(model);
  EXPECT_EQ(config.DefaultProvider(), "");
}

TEST(GenAIConfigDefaultProviderTest, EmptyWhenProviderOptionsEmpty) {
  GenAIConfig config;
  GenAIConfig::OnnxModel model;
  GenAIConfig::OnnxModel::Decoder decoder;
  decoder.session_options = GenAIConfig::OnnxModel::Decoder::SessionOptions{};
  model.decoder = std::move(decoder);
  config.model = std::move(model);
  EXPECT_EQ(config.DefaultProvider(), "");
}

// ========================================================================
// LoadFromFile
// ========================================================================

TEST_F(GenAIConfigTest, LoadFullConfig) {
  auto path = WriteFile("genai_config.json", R"({
    "model": {
      "context_length": 4096,
      "type": "phi3",
      "prompt_templates": {
        "system": "<|system|>\n{system}<|end|>",
        "user": "<|user|>\n{user}<|end|>"
      },
      "decoder": {
        "session_options": {
          "provider_options": [
            {"dml": "default"}
          ]
        }
      }
    },
    "search": {
      "max_length": 2048
    }
  })");

  auto config = GenAIConfig::LoadFromFile(path);

  ASSERT_TRUE(config.model.has_value());
  EXPECT_EQ(config.model->context_length, 4096);
  EXPECT_EQ(config.model->type, "phi3");
  EXPECT_EQ(config.model->prompt_templates.size(), 2);
  EXPECT_EQ(config.model->prompt_templates.at("system"), "<|system|>\n{system}<|end|>");
  EXPECT_EQ(config.DefaultProvider(), "dml");

  ASSERT_TRUE(config.search.has_value());
  EXPECT_EQ(config.search->max_length, 2048);
}

TEST_F(GenAIConfigTest, LoadMinimalConfig) {
  auto path = WriteFile("genai_config.json", R"({})");

  auto config = GenAIConfig::LoadFromFile(path);
  EXPECT_FALSE(config.model.has_value());
  EXPECT_FALSE(config.search.has_value());
}

TEST_F(GenAIConfigTest, LoadMissingOptionalFields) {
  auto path = WriteFile("genai_config.json", R"({
    "model": {
      "type": "gpt2"
    }
  })");

  auto config = GenAIConfig::LoadFromFile(path);
  ASSERT_TRUE(config.model.has_value());
  EXPECT_EQ(config.model->type, "gpt2");
  EXPECT_EQ(config.model->context_length, 0);
  EXPECT_TRUE(config.model->prompt_templates.empty());
  EXPECT_FALSE(config.model->decoder.has_value());
}

TEST_F(GenAIConfigTest, LoadThrowsForMissingFile) {
  EXPECT_THROW(GenAIConfig::LoadFromFile("/nonexistent/path/genai_config.json"),
               fl::Exception);
}

TEST_F(GenAIConfigTest, LoadThrowsForInvalidJson) {
  auto path = WriteFile("genai_config.json", "not json at all {{{");
  EXPECT_THROW(GenAIConfig::LoadFromFile(path), fl::Exception);
}

TEST_F(GenAIConfigTest, IgnoresNonObjectProviderOptions) {
  auto path = WriteFile("genai_config.json", R"({
    "model": {
      "decoder": {
        "session_options": {
          "provider_options": ["not_an_object", 42]
        }
      }
    }
  })");

  auto config = GenAIConfig::LoadFromFile(path);
  ASSERT_TRUE(config.model.has_value());
  ASSERT_TRUE(config.model->decoder.has_value());
  ASSERT_TRUE(config.model->decoder->session_options.has_value());
  EXPECT_TRUE(config.model->decoder->session_options->provider_options.empty());
}

// ========================================================================
// hidden_size (embeddings)
// ========================================================================

TEST_F(GenAIConfigTest, HiddenSize_Present) {
  auto path = WriteFile("genai_config.json", R"({
    "model": {
      "type": "bge",
      "hidden_size": 1024
    }
  })");

  auto config = GenAIConfig::LoadFromFile(path);
  ASSERT_TRUE(config.hidden_size.has_value());
  EXPECT_EQ(*config.hidden_size, 1024);
}

TEST_F(GenAIConfigTest, HiddenSize_Absent) {
  auto path = WriteFile("genai_config.json", R"({
    "model": {
      "type": "bge"
    }
  })");

  auto config = GenAIConfig::LoadFromFile(path);
  EXPECT_FALSE(config.hidden_size.has_value());
}

TEST_F(GenAIConfigTest, HiddenSize_WrongType_Ignored) {
  // Parser uses is_number_integer() — non-integer values are silently ignored.
  auto path = WriteFile("genai_config.json", R"({
    "model": {
      "type": "bge",
      "hidden_size": "bad"
    }
  })");

  auto config = GenAIConfig::LoadFromFile(path);
  EXPECT_FALSE(config.hidden_size.has_value());
}
