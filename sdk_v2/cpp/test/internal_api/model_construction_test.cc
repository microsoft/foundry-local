// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "exception.h"
#include "internal_api/test_helpers.h"
#include "model.h"
#include "model_info.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

using namespace fl;

static_assert(!std::is_default_constructible_v<Model>);
static_assert(std::is_nothrow_move_constructible_v<Model>);
static_assert(std::is_nothrow_move_assignable_v<Model>);

namespace {

Model MakeLeaf(std::string model_id = "test-model", std::string task = "chat-completion") {
  static fl::test::FakeServiceBindings svc;
  ModelInfo info;
  info.model_id = std::move(model_id);
  info.name = "test";
  info.version = 1;
  info.alias = "test-alias";
  info.task = std::move(task);
  return Model::FromModelInfo(std::move(info), "", svc.download_manager, svc.model_load_manager);
}

}  // namespace

TEST(ModelConstructionTest, LeafHasMetadataImmediately) {
  auto model = MakeLeaf();

  EXPECT_EQ(model.Info().model_id, "test-model");
  EXPECT_EQ(model.Info().alias, "test-alias");
  EXPECT_EQ(model.Info().task, "chat-completion");
}

TEST(ModelConstructionTest, LocalRegistrationHasMetadataImmediately) {
  static fl::test::FakeServiceBindings svc;
  ModelInfo info;
  info.model_id = "local-model:1";
  info.name = "local-model";
  info.version = 1;
  info.alias = "local-model";
  info.task = "chat-completion";

  auto model = Model::FromLocalRegistration(std::move(info), "model-path", svc.download_manager,
                                            svc.model_load_manager);

  EXPECT_EQ(model.Info().model_id, "local-model:1");
  EXPECT_EQ(model.Info().task, "chat-completion");
  EXPECT_EQ(model.Id(), "local-model:1");
}

TEST(ModelConstructionTest, ContainerHasSelectedMetadataImmediately) {
  auto container = Model::MakeContainer(MakeLeaf());

  ASSERT_TRUE(container.IsContainer());
  EXPECT_EQ(container.Info().model_id, "test-model");
  EXPECT_EQ(container.Info().task, "chat-completion");
}

TEST(ModelConstructionTest, MoveConstructionPreservesContainerMetadata) {
  auto source = Model::MakeContainer(MakeLeaf());

  Model moved(std::move(source));

  ASSERT_TRUE(moved.IsContainer());
  EXPECT_EQ(moved.Info().model_id, "test-model");
  EXPECT_EQ(moved.Info().task, "chat-completion");
}

TEST(ModelConstructionTest, MoveAssignmentPreservesContainerMetadata) {
  auto source = Model::MakeContainer(MakeLeaf());
  auto destination = MakeLeaf("destination-model");

  destination = std::move(source);

  ASSERT_TRUE(destination.IsContainer());
  EXPECT_EQ(destination.Info().model_id, "test-model");
  EXPECT_EQ(destination.Info().task, "chat-completion");
}

TEST(ModelConstructionTest, MakeContainerRejectsContainerAsVariant) {
  auto nested = Model::MakeContainer(MakeLeaf());

  EXPECT_THROW(Model::MakeContainer(std::move(nested)), fl::Exception);
}

TEST(ModelConstructionTest, AddVariantRejectsContainerAndPreservesExistingSelection) {
  auto container = Model::MakeContainer(MakeLeaf());
  auto nested = Model::MakeContainer(MakeLeaf("nested-model"));

  EXPECT_THROW(container.AddVariant(std::move(nested)), fl::Exception);

  ASSERT_TRUE(container.IsContainer());
  EXPECT_EQ(container.Variants().size(), 1u);
  EXPECT_EQ(container.Info().model_id, "test-model");
}
