// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Model integration tests — catalog, model info, download, load/unload.
// Uses the shared ModelFixture to avoid redundant Manager/download/load per test.
#include "model_fixture.h"
#include "utils/safe_getenv.h"

TEST_F(ModelFixture, ModelInfoAllPropertyAccessorsSucceed) {
  auto info = chat_model().GetInfo();

  EXPECT_FALSE(info.Id().empty());
  EXPECT_FALSE(info.Name().empty());
  EXPECT_FALSE(info.Alias().empty());
  EXPECT_FALSE(info.Uri().empty());

  EXPECT_EQ(info.DisplayName(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR));
  EXPECT_EQ(info.ModelType(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR));
  EXPECT_EQ(info.Publisher(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR));
  EXPECT_EQ(info.License(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR));
  EXPECT_EQ(info.LicenseDescription(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR));
  EXPECT_EQ(info.Task(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR));
  EXPECT_EQ(info.ModelProvider(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR));
  EXPECT_EQ(info.MinFlVersion(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR));
  EXPECT_EQ(info.ParentUri(), info.GetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_PARENT_URI_STR));

  {
    int64_t raw = info.GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT);
    std::optional<bool> expected = raw < 0 ? std::nullopt : std::optional<bool>{raw != 0};
    EXPECT_EQ(info.SupportsToolCalling(), expected);
  }
  {
    int64_t raw = info.GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT);
    std::optional<int64_t> expected = raw < 0 ? std::nullopt : std::optional<int64_t>{raw};
    EXPECT_EQ(info.FilesizeMb(), expected);
  }
  {
    int64_t raw = info.GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT);
    std::optional<int64_t> expected = raw < 0 ? std::nullopt : std::optional<int64_t>{raw};
    EXPECT_EQ(info.MaxOutputTokens(), expected);
  }
  EXPECT_EQ(info.CreatedAtUnix(), info.GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, 0));
  EXPECT_EQ(info.IsTestModel(),
            info.GetIntProperty(FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT, 0) != 0);

  EXPECT_FALSE(info.GetPromptTemplate("definitely_missing_prompt_template_key").has_value());
  EXPECT_FALSE(info.GetModelSetting("definitely_missing_model_setting_key").has_value());

  EXPECT_NO_THROW((void)info.Version());
  EXPECT_NO_THROW((void)info.DeviceType());
  EXPECT_NO_THROW((void)info.ExecutionProvider());
}

// --- Catalog and Download Validation ---

TEST_F(ModelFixture, CatalogValidation) {
  const auto& models = model_list();

  std::cout << "\n=== Catalog Validation ==="
            << "\nTotal models in catalog: " << models.size()
            << "\n";

  ASSERT_GT(models.size(), 10u)
      << "Expected 10+ models from the public catalog, got " << models.size();

  // Verify the selected model has expected properties
  auto info = chat_model().GetInfo();
  std::cout << "Selected model: " << info.Name()
            << "\nAlias:          " << info.Alias()
            << "\nURI:            " << info.Uri()
            << "\nPublisher:      " << info.Publisher().value_or("(none)")
            << "\n====================================\n";

  // Model should be cached (fixture already downloaded it)
  EXPECT_TRUE(chat_model().IsCached())
      << "Model should be cached after download";

  std::string local_path(chat_model().GetPath());
  EXPECT_TRUE(fs::exists(local_path))
      << "Download path does not exist: " << local_path;

  // Verify the path is under the expected default cache directory when no
  // override is active. When FOUNDRY_TEST_DATA_DIR is set, the path will be
  // under that directory instead.
  if (fl::test::SafeGetEnv("FOUNDRY_TEST_DATA_DIR").empty()) {
    std::string expected_fragment = ".foundry_local_sdk_test";
    bool path_contains_fragment = local_path.find(expected_fragment) != std::string::npos;
    EXPECT_TRUE(path_contains_fragment)
        << "Path should contain '" << expected_fragment << "', got: " << local_path;
  }

  // Find inference_model.json (may be in root or variant subdirectory)
  fs::path inference_model_path;
  for (const auto& entry : fs::recursive_directory_iterator(local_path)) {
    if (entry.is_regular_file() && entry.path().filename() == "inference_model.json") {
      inference_model_path = entry.path();
      break;
    }
  }

  EXPECT_FALSE(inference_model_path.empty())
      << "inference_model.json not found under " << local_path;

  if (!inference_model_path.empty()) {
    std::string json_text = ReadFileContents(inference_model_path);
    nlohmann::json doc;
    EXPECT_NO_THROW(doc = nlohmann::json::parse(json_text))
        << "inference_model.json is not valid JSON";

    if (doc.is_object()) {
      EXPECT_TRUE(doc.contains("Name"))
          << "inference_model.json missing 'Name' field";
      if (doc.contains("Name")) {
        // The on-disk Name field is the model id ("<name>:<version>"), not the bare catalog name.
        EXPECT_EQ(doc["Name"].get<std::string>(), std::string(info.Id()))
            << "Name mismatch between inference_model.json and catalog";
      }
    }
  }

  // Verify no download.tmp signal file remains
  bool found_signal = false;
  for (const auto& entry : fs::recursive_directory_iterator(local_path)) {
    if (entry.is_regular_file() && entry.path().filename() == "download.tmp") {
      found_signal = true;
      break;
    }
  }

  EXPECT_FALSE(found_signal)
      << "download.tmp should be removed after a complete download";

  std::cout << "\nDownload path:          " << local_path
            << "\ninference_model.json:   " << inference_model_path.string()
            << "\n====================================\n";
}

TEST_F(ModelFixture, DownloadCacheHit) {
  // Re-downloading a cached model should report progress exactly once (~100%)
  float cached_progress = -1.0f;
  int cached_callbacks = 0;

  chat_model().Download([&](float pct) -> int {
    ++cached_callbacks;
    cached_progress = pct;
    return 0;
  });

  EXPECT_EQ(cached_callbacks, 1)
      << "Cached model should report progress exactly once (100%)";
  EXPECT_GE(cached_progress, 99.0f)
      << "Cached model progress should be ~100%";
}

TEST_F(ModelFixture, LoadUnloadCycle) {
  auto info = chat_model().GetInfo();

  // Model is already loaded by the fixture — verify initial state
  EXPECT_TRUE(chat_model().IsLoaded())
      << "Model should be loaded by fixture";

  // Verify the model appears in GetLoadedModels
  {
    foundry_local::ModelList loaded_models = catalog().GetLoadedModels();

    EXPECT_GE(loaded_models.size(), 1u)
        << "At least one model should be loaded";

    bool found_loaded = false;
    for (const auto& m : loaded_models) {
      if (m->GetInfo().Name() == info.Name()) {
        found_loaded = true;
        break;
      }
    }

    EXPECT_TRUE(found_loaded)
        << "Loaded models list should contain '" << info.Name() << "'";
  }

  // Unload the model
  chat_model().Unload();

  EXPECT_FALSE(chat_model().IsLoaded())
      << "Model should not be loaded after Unload()";

  // Verify the model is no longer in GetLoadedModels
  {
    foundry_local::ModelList loaded_after_unload = catalog().GetLoadedModels();

    bool still_loaded = false;
    for (const auto& m : loaded_after_unload) {
      if (m->GetInfo().Name() == info.Name()) {
        still_loaded = true;
        break;
      }
    }

    EXPECT_FALSE(still_loaded)
        << "Model '" << info.Name() << "' should not appear in loaded models after Unload()";
  }

  // Re-load the model so subsequent tests still have it available
  chat_model().Load();

  EXPECT_TRUE(chat_model().IsLoaded())
      << "Model should be loaded after re-Load()";
}

// --- Tool Calling Tests ---

TEST_F(ModelFixture, ToolCallWithRequired) {
  using namespace foundry_local;

  ChatSession session(chat_model());

  session.AddToolDefinition(ToolDefinition{
      "multiply_numbers",
      "A tool for multiplying two numbers.",
      R"({
        "type": "object",
        "properties": {
          "first": { "type": "integer", "description": "The first number in the operation" },
          "second": { "type": "integer", "description": "The second number in the operation" }
        },
        "required": ["first", "second"]
      })"});

  Request request{
      SystemMessage("You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question."),
      UserMessage("What is the answer to 7 multiplied by 6?"),
  };
  RequestOptions opts;
  opts.search.temperature = 0.0f;
  opts.search.max_output_tokens = 256;
  opts.tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED;
  request.SetOptions(opts);

  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_TOOL_CALLS)
      << "Expected tool_calls finish reason";

  // Find the tool call item in the response
  bool found_tool_call = false;
  for (const auto& item : response.GetItems()) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
      auto tc = item.GetToolCall();
      EXPECT_EQ(tc.name, "multiply_numbers")
          << "Tool call name mismatch. Got: " << tc.name;
      EXPECT_FALSE(tc.arguments.empty());
      std::cout << "Tool call: " << tc.name << "(" << tc.arguments << ")\n";
      found_tool_call = true;
      break;
    }
  }

  EXPECT_TRUE(found_tool_call) << "No TOOL_CALL item in response";
}
