// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/request_budget.h"

#include "exception.h"
#include "inferencing/generative/chat/search_options.h"

#include <gtest/gtest.h>

#include <limits>

namespace fl {
namespace {

TEST(RequestBudgetTest, ExactBoundariesAtNinetyNinetyNineOneHundredAndOneHundredOne) {
  const auto ninety = ComputeRequestBudget(90, 10, 100);
  EXPECT_EQ(ninety.required_tokens, 100);
  EXPECT_TRUE(ninety.fits);
  EXPECT_EQ(ninety.deficit_tokens, 0);

  const auto ninety_nine = ComputeRequestBudget(99, 1, 100);
  EXPECT_EQ(ninety_nine.required_tokens, 100);
  EXPECT_TRUE(ninety_nine.fits);

  const auto one_hundred = ComputeRequestBudget(100, 1, 100);
  EXPECT_EQ(one_hundred.required_tokens, 101);
  EXPECT_FALSE(one_hundred.fits);
  EXPECT_EQ(one_hundred.deficit_tokens, 1);

  const auto one_hundred_one = ComputeRequestBudget(101, 1, 100);
  EXPECT_EQ(one_hundred_one.required_tokens, 102);
  EXPECT_FALSE(one_hundred_one.fits);
  EXPECT_EQ(one_hundred_one.deficit_tokens, 2);
}

TEST(RequestBudgetTest, RejectsInvalidOutputReserveAndCheckedAdditionOverflow) {
  EXPECT_THROW(ComputeRequestBudget(1, 0, 100), Exception);
  EXPECT_THROW(ComputeRequestBudget((std::numeric_limits<int64_t>::max)(), 1, 100), Exception);
}

TEST(RequestBudgetTest, ResolvedTextFallbackFitsExactlyAndRejectsOneTokenOver) {
  const auto output_limit = ResolveOutputLimit(SearchOptions{}, false);

  const auto exact = ComputeRequestBudget(2048, output_limit, 4096);
  EXPECT_EQ(exact.required_tokens, 4096);
  EXPECT_TRUE(exact.fits);

  const auto one_over = ComputeRequestBudget(2049, output_limit, 4096);
  EXPECT_EQ(one_over.required_tokens, 4097);
  EXPECT_FALSE(one_over.fits);
  EXPECT_EQ(one_over.deficit_tokens, 1);
}

TEST(RequestBudgetTest, PositiveModelContextLengthWinsWithPositiveSearchFallback) {
  GenAIConfig config;
  config.model.emplace();
  config.model->context_length = 4096;
  config.search.emplace();
  config.search->max_length = 2048;
  EXPECT_EQ(GetModelMaxContextLength(config), 4096);

  config.model->context_length = 0;
  EXPECT_EQ(GetModelMaxContextLength(config), 2048);

  config.search->max_length = 0;
  EXPECT_THROW(GetModelMaxContextLength(config), Exception);
}

}  // namespace
}  // namespace fl
