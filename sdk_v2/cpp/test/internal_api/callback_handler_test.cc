// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for CallbackHandler — verifies the worker thread tolerates user-callback
// exceptions without crashing the process or hanging the destructor.

#include "inferencing/session/callback_handler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "inferencing/session/request.h"
#include "internal_api/test_helpers.h"
#include "items/text_item.h"

using namespace fl;

namespace {

CallbackHandler::CallbackFn MakeThrowingCallback(std::atomic<int>& invocations) {
  return [&invocations](flStreamingCallbackData, void*) -> int {
    ++invocations;
    throw std::runtime_error("boom");
  };
}

}  // namespace

TEST(CallbackHandlerTest, StdExceptionFromCallbackDoesNotTerminate) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  std::atomic<int> invocations{0};

  {
    CallbackHandler handler(request, MakeThrowingCallback(invocations), fl::test::NullLog());
    handler.PushItem(std::make_unique<TextItem>("first"));

    // Give the worker a moment to observe the item, fire the callback, and catch.
    handler.Drain();
  }

  EXPECT_GE(invocations.load(), 1);
  EXPECT_TRUE(request.IsCancellationRequested());
  EXPECT_EQ(request.GetCancellationReason(), Request::CancellationReason::StreamingCallbackException);
  EXPECT_EQ(request.CancellationDetail(), "boom");
}

TEST(CallbackHandlerTest, NonStdExceptionFromCallbackDoesNotTerminate) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  std::atomic<int> invocations{0};

  auto fn = [&invocations](flStreamingCallbackData, void*) -> int {
    ++invocations;
    throw 42;  // non-std exception
  };

  {
    CallbackHandler handler(request, fn, fl::test::NullLog());
    handler.PushItem(std::make_unique<TextItem>("first"));
    handler.Drain();
  }

  EXPECT_GE(invocations.load(), 1);
  EXPECT_TRUE(request.IsCancellationRequested());
  EXPECT_EQ(request.GetCancellationReason(), Request::CancellationReason::StreamingCallbackException);
  EXPECT_EQ(request.CancellationDetail(), "non-standard exception");
}

TEST(CallbackHandlerTest, FurtherPushesAfterExceptionAreNoOps) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  std::atomic<int> invocations{0};

  CallbackHandler handler(request, MakeThrowingCallback(invocations), fl::test::NullLog());
  handler.PushItem(std::make_unique<TextItem>("first"));

  // Wait until the worker has cancelled the request after catching the throw.
  for (int i = 0; i < 200 && !request.IsCancellationRequested(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  ASSERT_TRUE(request.IsCancellationRequested());

  const int invocations_after_first = invocations.load();

  // Subsequent pushes must be dropped (canceled is set, so PushItem skips).
  handler.PushItem(std::make_unique<TextItem>("second"));
  handler.PushItem(std::make_unique<TextItem>("third"));

  handler.DrainPending();
  handler.Drain();

  // Worker exited after the throw — no further callback invocations.
  EXPECT_EQ(invocations.load(), invocations_after_first);
}

TEST(CallbackHandlerTest, NormalCallbackCancelsViaReturnValue) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  std::atomic<int> invocations{0};

  auto fn = [&invocations, &request](flStreamingCallbackData data, void*) -> int {
    ++invocations;
    // Pop one item per the established contract so the worker doesn't loop on us.
    auto* queue = reinterpret_cast<ItemQueue*>(data.item_queue);
    (void)queue->TryPop();
    // Returning non-zero asks the session to cancel.
    (void)request;
    return 1;
  };

  CallbackHandler handler(request, fn, fl::test::NullLog());
  handler.PushItem(std::make_unique<TextItem>("hello"));
  handler.Drain();

  EXPECT_EQ(invocations.load(), 1);
  EXPECT_TRUE(request.IsCancellationRequested());
  EXPECT_EQ(request.GetCancellationReason(), Request::CancellationReason::StreamingCallback);
}

TEST(CallbackHandlerTest, DrainPendingWaitsForDeliveryWithoutClosingTheQueue) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  std::atomic<int> invocations{0};

  auto fn = [&invocations](flStreamingCallbackData data, void*) -> int {
    auto* queue = reinterpret_cast<ItemQueue*>(data.item_queue);
    (void)queue->TryPop();
    ++invocations;
    return 0;
  };

  CallbackHandler handler(request, fn, fl::test::NullLog());
  handler.PushItem(std::make_unique<TextItem>("content"));
  handler.DrainPending();
  EXPECT_EQ(invocations.load(), 1);

  handler.PushItem(std::make_unique<TextItem>("terminal"));
  handler.Drain();
  EXPECT_EQ(invocations.load(), 2);
}

TEST(CallbackHandlerTest, CancellationDrainsSmallBufferedBacklog) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  std::atomic<int> invocations{0};
  std::mutex mutex;
  std::condition_variable cv;
  bool callback_started = false;
  bool release_callback = false;

  auto fn = [&](flStreamingCallbackData data, void*) -> int {
    auto* queue = reinterpret_cast<ItemQueue*>(data.item_queue);
    (void)queue->TryPop();
    ++invocations;

    std::unique_lock<std::mutex> lock(mutex);
    callback_started = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release_callback; });
    return 0;
  };

  CallbackHandler handler(request, fn, fl::test::NullLog());
  handler.PushItem(std::make_unique<TextItem>("first"));
  bool started = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    started = cv.wait_for(lock, std::chrono::seconds(2), [&] { return callback_started; });
  }
  EXPECT_TRUE(started);

  for (size_t i = 0; i < 3; ++i) {
    handler.PushItem(std::make_unique<TextItem>("buffered"));
  }
  request.Cancel();
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_callback = true;
  }
  cv.notify_all();

  handler.Drain();
  EXPECT_EQ(invocations.load(), 4);
}

TEST(CallbackHandlerTest, CancellationDropsLargeBufferedBacklog) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  std::atomic<int> invocations{0};
  std::mutex mutex;
  std::condition_variable cv;
  bool callback_started = false;
  bool release_callback = false;

  auto fn = [&](flStreamingCallbackData data, void*) -> int {
    auto* queue = reinterpret_cast<ItemQueue*>(data.item_queue);
    (void)queue->TryPop();
    ++invocations;

    std::unique_lock<std::mutex> lock(mutex);
    callback_started = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release_callback; });
    return 0;
  };

  CallbackHandler handler(request, fn, fl::test::NullLog());
  handler.PushItem(std::make_unique<TextItem>("first"));
  bool started = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    started = cv.wait_for(lock, std::chrono::seconds(2), [&] { return callback_started; });
  }
  EXPECT_TRUE(started);

  for (size_t i = 0; i <= CallbackHandler::kMaxCancellationDrainItems; ++i) {
    handler.PushItem(std::make_unique<TextItem>("buffered"));
  }
  request.Cancel();
  {
    std::lock_guard<std::mutex> lock(mutex);
    release_callback = true;
  }
  cv.notify_all();

  handler.DrainPending();
  handler.Drain();
  EXPECT_EQ(invocations.load(), 1);
}
