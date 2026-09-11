// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for the HTTP status / error-type mapping the Chat Completions and Responses handlers share.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/handler_utils.h"

#include "exception.h"

#include <gtest/gtest.h>

#include <string>

using namespace fl;

namespace {

/// Build the exception the inference path raises for the given error code, exactly as FL_THROW would.
fl::Exception MakeException(flErrorCode code, const std::string& message) {
  return fl::Exception(fl::CodeLocation{__FILE__, __LINE__, __func__}, message, code);
}

}  // namespace

TEST(HandlerErrorMappingTest, InvalidArgumentIsAClientError) {
  // Everything the transcript rejects about a caller's conversation uses INVALID_ARGUMENT: unknown, duplicate, and
  // already-answered tool call IDs, and supplied tool arguments that are not a JSON object.
  const auto status = StatusForException(
      MakeException(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                    "tool result references unknown tool call id 'call_missing'"));

  EXPECT_EQ(status.code, Status::CODE_400.code);
  EXPECT_STREQ(ErrorTypeForStatus(status), "invalid_request_error");
}

TEST(HandlerErrorMappingTest, DuplicateCallIdIsAClientError) {
  const auto status = StatusForException(
      MakeException(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                    "tool call id 'call_1' is already used by another tool call"));

  EXPECT_EQ(status.code, Status::CODE_400.code);
}

TEST(HandlerErrorMappingTest, MalformedSuppliedArgumentsAreAClientError) {
  const auto status = StatusForException(
      MakeException(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                    "tool call 'get_weather' has arguments that are not a JSON object: [1,2]"));

  EXPECT_EQ(status.code, Status::CODE_400.code);
}

TEST(HandlerErrorMappingTest, InternalFailuresRemainServerErrors) {
  const auto status = StatusForException(MakeException(FOUNDRY_LOCAL_ERROR_INTERNAL, "token generation failed"));

  EXPECT_EQ(status.code, Status::CODE_500.code);
  EXPECT_STREQ(ErrorTypeForStatus(status), "server_error");
}

TEST(HandlerErrorMappingTest, OtherErrorCodesRemainServerErrors) {
  // Only invalid-argument is reclassified; existing handling for every other code is preserved.
  for (auto code : {FOUNDRY_LOCAL_ERROR_INVALID_USAGE, FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED,
                    FOUNDRY_LOCAL_ERROR_NETWORK}) {
    EXPECT_EQ(StatusForException(MakeException(code, "failure")).code, Status::CODE_500.code)
        << "error code: " << static_cast<int>(code);
  }
}

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
