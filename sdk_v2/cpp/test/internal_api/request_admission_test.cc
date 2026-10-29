// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/request_admission.h"
#include "exception.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

namespace {

TEST(RequestAdmissionTest, IdleCancellationDoesNotCancelTheNextInvocation) {
  fl::Request request;
  fl::api::CancelRequest(request);
  int cancellations = 0;
  {
    fl::api::RequestAdmission admission(request, [&](auto, const auto&) { ++cancellations; });
    EXPECT_FALSE(request.IsCancellationRequested());
    fl::api::CancelRequest(request);
    fl::api::CancelRequest(request);
    EXPECT_TRUE(request.IsCancellationRequested());
    EXPECT_EQ(cancellations, 1);
  }

  EXPECT_TRUE(request.IsCompleted());
  fl::api::CancelRequest(request);
  EXPECT_EQ(cancellations, 1);
  fl::api::RequestAdmission reused(request, [&](auto, const auto&) { ++cancellations; });
  EXPECT_FALSE(request.IsCancellationRequested());
}

TEST(RequestAdmissionTest, RetainsEmbeddedCancellationWhenThereIsNoTransportAdmission) {
  fl::Request request;
  ASSERT_TRUE(request.TryBegin());
  fl::api::CancelRequest(request);
  EXPECT_EQ(request.GetCancellationReason(), fl::Request::CancellationReason::Caller);
}

TEST(RequestAdmissionTest, RejectsConcurrentReuseWithoutLosingTheFirstAdmission) {
  fl::Request request;
  int first = 0;
  fl::api::RequestAdmission admission(request, [&](auto, const auto&) { ++first; });
  EXPECT_THROW((fl::api::RequestAdmission(request, {})), fl::Exception);
  fl::api::CancelRequest(request);
  EXPECT_EQ(first, 1);
}

TEST(RequestAdmissionTest, CancellationRunsOutsideTheAdmissionLockAndPreservesTheWinningReason) {
  fl::Request request;
  fl::Request::CancellationReason observed = fl::Request::CancellationReason::None;
  std::string observed_detail;
  fl::api::RequestAdmission admission(request, [&](auto reason, const auto& detail) {
    observed = reason;
    observed_detail = detail;
    fl::api::CancelRequest(request);
  });
  fl::api::CancelRequest(request, fl::Request::CancellationReason::StreamingCallbackException, "callback failed");
  EXPECT_EQ(observed, fl::Request::CancellationReason::StreamingCallbackException);
  EXPECT_EQ(observed_detail, "callback failed");
}

TEST(RequestAdmissionTest, LateCancellationActionCannotTargetAReusedRequest) {
  fl::Request request;
  std::promise<void> entered;
  std::promise<void> resume;
  auto resume_future = resume.get_future().share();
  int new_cancellations = 0;
  auto first = std::make_unique<fl::api::RequestAdmission>(request, [&](auto, const auto&) {
    entered.set_value();
    resume_future.wait();
  });
  std::thread cancellation([&] { fl::api::CancelRequest(request); });
  const auto status = entered.get_future().wait_for(std::chrono::seconds(5));
  if (status != std::future_status::ready) {
    resume.set_value();
    cancellation.join();
    FAIL() << "cancellation action was not entered";
  }

  first.reset();
  fl::api::RequestAdmission second(request, [&](auto, const auto&) { ++new_cancellations; });
  resume.set_value();
  cancellation.join();
  EXPECT_FALSE(request.IsCancellationRequested());
  EXPECT_EQ(new_cancellations, 0);
  fl::api::CancelRequest(request);
  EXPECT_EQ(new_cancellations, 1);
}

}  // namespace
