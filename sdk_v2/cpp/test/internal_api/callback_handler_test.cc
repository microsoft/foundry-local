// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for CallbackHandler — verifies the worker thread tolerates user-callback
// exceptions without crashing the process or hanging the destructor.

#include "inferencing/session/callback_handler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
  std::atomic<int> invocations{0};

  {
    CallbackHandler handler(request, MakeThrowingCallback(invocations), fl::test::NullLog());
    handler.PushItem(std::make_unique<TextItem>("first"));

    // Give the worker a moment to observe the item, fire the callback, and catch.
    handler.Drain();
  }

  EXPECT_GE(invocations.load(), 1);
  EXPECT_TRUE(request.canceled.load());
}

TEST(CallbackHandlerTest, NonStdExceptionFromCallbackDoesNotTerminate) {
  Request request;
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
  EXPECT_TRUE(request.canceled.load());
}

TEST(CallbackHandlerTest, FurtherPushesAfterExceptionAreNoOps) {
  Request request;
  std::atomic<int> invocations{0};

  CallbackHandler handler(request, MakeThrowingCallback(invocations), fl::test::NullLog());
  handler.PushItem(std::make_unique<TextItem>("first"));

  // Wait until the worker has cancelled the request after catching the throw.
  for (int i = 0; i < 200 && !request.canceled.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  ASSERT_TRUE(request.canceled.load());

  const int invocations_after_first = invocations.load();

  // Subsequent pushes must be dropped (canceled is set, so PushItem skips).
  handler.PushItem(std::make_unique<TextItem>("second"));
  handler.PushItem(std::make_unique<TextItem>("third"));

  handler.Drain();

  // Worker exited after the throw — no further callback invocations.
  EXPECT_EQ(invocations.load(), invocations_after_first);
}

TEST(CallbackHandlerTest, NormalCallbackCancelsViaReturnValue) {
  Request request;
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
  EXPECT_TRUE(request.canceled.load());
}

TEST(CallbackHandlerTest, FinalItemIsDeliveredAfterOperationHasStopped) {
  auto state = std::make_shared<OperationState>(
      std::weak_ptr<SessionRuntime>{}, std::weak_ptr<RequestControl>{}, std::nullopt,
      std::chrono::milliseconds(0));
  OperationContext operation(state);
  ASSERT_TRUE(state->RequestStop(StopReason::kExternalCancel));

  std::mutex mutex;
  std::string delivered_text;

  auto callback = [&](flStreamingCallbackData data, void*) -> int {
    auto* queue = reinterpret_cast<ItemQueue*>(data.item_queue);
    auto item = queue->TryPop();
    if (item && item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        delivered_text = static_cast<TextItem&>(*item).text;
      }
    }

    return 0;
  };

  CallbackHandler handler(operation, callback, fl::test::NullLog());
  handler.PushItem(std::make_unique<TextItem>("dropped"));
  handler.PushFinalItem(std::make_unique<TextItem>("cancelled-terminal"));
  handler.Quiesce();

  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(delivered_text, "cancelled-terminal");
}
