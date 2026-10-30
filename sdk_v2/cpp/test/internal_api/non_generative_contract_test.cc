// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contracts/non_generative.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace fl {
namespace {

TEST(NonGenerativeContractTest, ParsesTypedRequest) {
  auto request = nlohmann::ordered_json{
      {"state", {{"enabled", true}}},
      {"temperature", 0.5},
      {"questions",
       {{"quality",
         {{"type", "choice"},
          {"instructions", "Choose"},
          {"criteria", {{"good", "Good"}, {"bad", "Bad"}}}}}}}}
                     .get<NonGenerativeRequest>();
  EXPECT_FLOAT_EQ(request.temperature, 0.5f);
  ASSERT_EQ(request.questions.size(), 1u);
  EXPECT_EQ(request.questions.front().first, "quality");
  EXPECT_EQ(request.questions.front().second.type, "choice");
}

TEST(NonGenerativeContractTest, RejectsEmptyQuestionsAndBadTemperature) {
  EXPECT_THROW((nlohmann::ordered_json{{"questions", nlohmann::ordered_json::object()}}
                    .get<NonGenerativeRequest>()),
               nlohmann::json::exception);
  EXPECT_THROW((nlohmann::ordered_json{{"temperature", 0},
                                       {"questions", {{"q", {{"type", "noul"}}}}}}
                    .get<NonGenerativeRequest>()),
               nlohmann::json::exception);
}

TEST(NonGenerativeContractTest, RejectsUnknownQuestionType) {
  EXPECT_THROW((nlohmann::ordered_json{{"questions", {{"q", {{"type", "freeform"}}}}}}
                    .get<NonGenerativeRequest>()),
               nlohmann::json::exception);
}

TEST(NonGenerativeContractTest, ParsesRankingRequest) {
  auto request = nlohmann::ordered_json{
      {"context", {{"text", "sample"}}},
      {"question", "Which answer is best?"},
      {"answers", {"first", "second"}},
      {"temperature", 0.75},
  }
                     .get<RankingRequest>();
  EXPECT_EQ(request.answers, (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(request.question, "Which answer is best?");
  EXPECT_FLOAT_EQ(request.temperature, 0.75f);
}

TEST(NonGenerativeContractTest, RejectsInvalidRankingAnswers) {
  EXPECT_THROW((nlohmann::ordered_json{{"answers", nlohmann::ordered_json::array()}}
                    .get<RankingRequest>()),
               nlohmann::json::exception);
  EXPECT_THROW((nlohmann::ordered_json{{"answers", {"valid", ""}}}
                    .get<RankingRequest>()),
               nlohmann::json::exception);
  EXPECT_THROW((nlohmann::ordered_json{{"model", ""}, {"answers", {"a", "b"}}}
                    .get<RankingRequest>()),
               nlohmann::json::exception);
}

TEST(NonGenerativeContractTest, AcceptsCatalogModelIdsAndAliases) {
  auto ranking = nlohmann::ordered_json{
      {"model", "clm-v0.1-8b-generic-cpu:1"}, {"answers", {"a", "b"}}}
                     .get<RankingRequest>();
  EXPECT_EQ(ranking.model, "clm-v0.1-8b-generic-cpu:1");

  auto decision = nlohmann::ordered_json{
      {"model", "kev-local"},
      {"questions", {{"q", {{"type", "noul"}}}}},
  }
                      .get<NonGenerativeRequest>();
  EXPECT_EQ(decision.model, "kev-local");
}

TEST(NonGenerativeContractTest, RejectsUnsignedStructuredOverflow) {
  EXPECT_THROW(
      nlohmann::ordered_json::parse(
          R"({"state":18446744073709551615,"questions":{"q":{"type":"noul"}}})")
          .get<NonGenerativeRequest>(),
      nlohmann::json::exception);
}

}  // namespace
}  // namespace fl
