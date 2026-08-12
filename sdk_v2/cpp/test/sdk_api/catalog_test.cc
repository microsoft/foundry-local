// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Catalog-focused SDK integration tests using only the public C++ API.

#include <memory>
#include <type_traits>

#include "model_fixture.h"

namespace {

const foundry_local::IModel* FindModelByAlias(
    const foundry_local::ModelList& models,
    std::string_view alias) {
  for (const auto& model : models) {
    if (model->GetInfo().Alias() == alias) {
      return model.get();
    }
  }

  return nullptr;
}

}  // namespace

TEST_F(ModelFixture, CatalogGetNameReturnsNonEmptyString) {
  auto name = catalog().GetName();
  EXPECT_FALSE(name.empty()) << "Catalog name should not be empty";
}

TEST_F(ModelFixture, CatalogGetModelByAliasReturnsExpectedModelAndVariants) {
  auto model = catalog().GetModel(model_alias());
  ASSERT_NE(model, nullptr) << "Expected catalog lookup by alias to succeed for '" << model_alias() << "'";

  auto info = model->GetInfo();
  EXPECT_EQ(info.Alias(), model_alias());
  EXPECT_EQ(info.Name(), chat_model().GetInfo().Name());
  EXPECT_EQ(info.Id(), model_id());
  EXPECT_FALSE(info.Uri().empty());

  foundry_local::ModelList variants = model->GetVariants();
  ASSERT_GE(variants.size(), 1u);

  bool found_expected_variant = false;
  for (const auto& variant : variants) {
    if (variant->GetInfo().Id() == model_id()) {
      found_expected_variant = true;
      break;
    }
  }

  EXPECT_TRUE(found_expected_variant)
      << "Expected alias lookup to expose variant '" << model_id() << "' in GetVariants()";
}

TEST_F(ModelFixture, CatalogGetModelReturnsNulloptForUnknownAlias) {
  auto model = catalog().GetModel("nonexistent_model_xyz");

  EXPECT_EQ(model, nullptr);
}

// Diagnostic dump: list every model in the catalog with its alias and the ids
// of all variants. Useful when triaging variant-selection or filtering issues
// — the GTEST output captures the catalog snapshot the test run was working
// against. Always passes; assertions are sanity checks only.
TEST_F(ModelFixture, CatalogDumpAllModelsAndVariants) {
  foundry_local::ModelList models = catalog().GetModels();

  ASSERT_GE(models.size(), 1u) << "Catalog should expose at least one model";

  std::cout << "Catalog '" << catalog().GetName() << "' contains " << models.size() << " model(s):\n";

  for (const auto& model : models) {
    auto info = model->GetInfo();
    std::cout << "  alias: " << info.Alias() << "\tTask:" << info.Task().value_or("not set") << "\n";

    foundry_local::ModelList variants = model->GetVariants();
    for (const auto& variant : variants) {
      std::cout << "    - " << variant->GetInfo().Id() << "\n";
    }
  }
}

TEST_F(ModelFixture, CatalogGetModelVariantByIdReturnsExpectedVariant) {
  auto variant = catalog().GetModelVariant(model_id());
  ASSERT_NE(variant, nullptr) << "Expected catalog lookup by id to succeed for '" << model_id() << "'";

  auto info = variant->GetInfo();
  EXPECT_EQ(info.Id(), model_id());
  EXPECT_EQ(info.Alias(), model_alias());
  EXPECT_EQ(info.Name(), chat_model().GetInfo().Name());
  EXPECT_TRUE(variant->IsCached());

  std::string local_path(variant->GetPath());
  EXPECT_FALSE(local_path.empty());
  EXPECT_TRUE(fs::exists(local_path)) << "Expected cached variant path to exist: " << local_path;

  foundry_local::ModelList variants = variant->GetVariants();
  ASSERT_EQ(variants.size(), 1u)
      << "A model returned by GetModelVariant(id) should only expose that single variant";
  EXPECT_EQ(variants.front()->GetInfo().Id(), model_id());
}

TEST_F(ModelFixture, CatalogGetModelVariantReturnsNulloptForUnknownId) {
  auto variant = catalog().GetModelVariant("nonexistent_model_xyz:999");

  EXPECT_EQ(variant, nullptr);
}

TEST_F(ModelFixture, CatalogGetCachedModelsIncludesDownloadedModel) {
  foundry_local::ModelList cached = catalog().GetCachedModels();

  ASSERT_GE(cached.size(), 1u) << "Expected at least one cached model after fixture download";

  const foundry_local::IModel* cached_model = FindModelByAlias(cached, model_alias());
  ASSERT_NE(cached_model, nullptr)
      << "Expected cached models list to contain the downloaded alias '" << model_alias() << "'";

  EXPECT_TRUE(cached_model->IsCached());
  EXPECT_EQ(cached_model->GetInfo().Id(), model_id());

  std::string local_path(cached_model->GetPath());
  EXPECT_FALSE(local_path.empty());
  EXPECT_TRUE(fs::exists(local_path)) << "Expected cached model path to exist: " << local_path;
}

// -----------------------------------------------------------------------------
// ModelInfo accessor surface tests.
//
// Exercises the optional<>-returning accessors and structural accessors added
// to ModelInfo. Uses the chat fixture model so we don't pull in any new model
// dependency. Per-property assertions are conditional on whether the catalog
// entry actually populates the property — the goal is to exercise the code
// paths and verify the returned types behave correctly, not to enforce that
// any one catalog entry sets every optional field.
// -----------------------------------------------------------------------------

TEST_F(ModelFixture, ModelInfoGetRuntimeMatchesDeviceAndExecutionProvider) {
  auto variant = catalog().GetModelVariant(model_id());
  ASSERT_NE(variant, nullptr);

  auto info = variant->GetInfo();
  auto runtime = info.GetRuntime();

  if (info.DeviceType() == FOUNDRY_LOCAL_DEVICE_NOTSET) {
    EXPECT_FALSE(runtime.has_value())
        << "GetRuntime() must return nullopt when DeviceType() is NOTSET";
    return;
  }

  ASSERT_TRUE(runtime.has_value())
      << "GetRuntime() must return a value when DeviceType() is set";
  EXPECT_EQ(runtime->device_type, info.DeviceType());
  EXPECT_EQ(runtime->execution_provider.has_value(), info.ExecutionProvider().has_value());
  if (runtime->execution_provider && info.ExecutionProvider()) {
    EXPECT_EQ(*runtime->execution_provider, *info.ExecutionProvider());
  }
}

TEST_F(ModelFixture, ModelInfoGetModelSettingsViewIsConsistentWithKeyedAccessor) {
  auto variant = catalog().GetModelVariant(model_id());
  ASSERT_NE(variant, nullptr);

  auto info = variant->GetInfo();
  auto settings = info.GetModelSettings();

  if (!settings.has_value()) {
    // Catalog entry has no settings -- keyed accessor must agree for arbitrary keys.
    EXPECT_FALSE(info.GetModelSetting("temperature").has_value());
    return;
  }

  auto entries = settings->GetAll();
  // GetModelSettings() returned a non-null view; if it's empty the view exists but has no entries.
  // Verify cross-consistency for every entry that the view exposes.
  for (const auto& entry : entries) {
    ASSERT_FALSE(entry.key.empty()) << "Settings key should not be empty";
    std::string key_str(entry.key);
    auto via_keyed = info.GetModelSetting(key_str.c_str());
    ASSERT_TRUE(via_keyed.has_value())
        << "Key '" << key_str << "' enumerated by GetModelSettings() must also be visible via GetModelSetting()";
    EXPECT_EQ(*via_keyed, entry.value)
        << "Value for key '" << key_str << "' must match between GetModelSettings() view and GetModelSetting()";
  }
}

TEST_F(ModelFixture, ModelInfoOptionalIntAccessorsHaveExpectedTypes) {
  auto variant = catalog().GetModelVariant(model_id());
  ASSERT_NE(variant, nullptr);

  auto info = variant->GetInfo();

  // Compile-time type checks: these accessors must return std::optional<>.
  static_assert(std::is_same_v<decltype(info.SupportsToolCalling()), std::optional<bool>>,
                "SupportsToolCalling() must return std::optional<bool>");
  static_assert(std::is_same_v<decltype(info.FilesizeMb()), std::optional<int64_t>>,
                "FilesizeMb() must return std::optional<int64_t>");
  static_assert(std::is_same_v<decltype(info.MaxOutputTokens()), std::optional<int64_t>>,
                "MaxOutputTokens() must return std::optional<int64_t>");
  static_assert(std::is_same_v<decltype(info.ContextLength()), std::optional<int64_t>>,
                "ContextLength() must return std::optional<int64_t>");
  static_assert(std::is_same_v<decltype(info.IsTestModel()), bool>,
                "IsTestModel() must return bool (no longer int64_t tristate)");

  // FilesizeMb: a downloaded variant should have a positive value if populated.
  if (auto fs_mb = info.FilesizeMb()) {
    EXPECT_GT(*fs_mb, 0)
        << "FilesizeMb() must be positive when populated";
  }

  if (auto mot = info.MaxOutputTokens()) {
    EXPECT_GT(*mot, 0) << "MaxOutputTokens() must be positive when populated";
  }

  if (auto ctx = info.ContextLength()) {
    EXPECT_GT(*ctx, 0) << "ContextLength() must be positive when populated";
  }

  // SupportsToolCalling / IsTestModel: just exercise the accessor; value depends on catalog.
  (void)info.SupportsToolCalling();
  (void)info.IsTestModel();
}

TEST_F(ModelFixture, ModelInfoOptionalStringModalityAccessorsAreNonEmptyWhenPopulated) {
  auto variant = catalog().GetModelVariant(model_id());
  ASSERT_NE(variant, nullptr);

  auto info = variant->GetInfo();

  static_assert(std::is_same_v<decltype(info.InputModalities()), std::optional<std::string_view>>,
                "InputModalities() must return std::optional<std::string_view>");
  static_assert(std::is_same_v<decltype(info.OutputModalities()), std::optional<std::string_view>>,
                "OutputModalities() must return std::optional<std::string_view>");
  static_assert(std::is_same_v<decltype(info.Capabilities()), std::optional<std::string_view>>,
                "Capabilities() must return std::optional<std::string_view>");

  if (auto im = info.InputModalities()) {
    EXPECT_FALSE(im->empty()) << "InputModalities() must be non-empty when populated";
  }

  if (auto om = info.OutputModalities()) {
    EXPECT_FALSE(om->empty()) << "OutputModalities() must be non-empty when populated";
  }

  if (auto caps = info.Capabilities()) {
    EXPECT_FALSE(caps->empty()) << "Capabilities() must be non-empty when populated";
  }
}
