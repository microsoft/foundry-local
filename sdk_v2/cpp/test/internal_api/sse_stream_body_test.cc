// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for SSE stream bodies and worker lifetime.
//

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "service/handler_utils.h"
#include "service/web_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

using namespace fl;

// ========================================================================
// SseStreamBody::Push + read basics
// ========================================================================

TEST(SseStreamBodyTest, PushAndReadSingleChunk) {
  SseStreamBody body;
  body.Push("data: {\"text\":\"hello\"}\n\n");
  body.Finish();

  char buffer[256] = {};
  oatpp::async::Action action;
  auto bytes_read = body.read(buffer, sizeof(buffer), action);

  ASSERT_GT(bytes_read, 0);
  std::string result(buffer, bytes_read);
  EXPECT_EQ(result, "data: {\"text\":\"hello\"}\n\n");
}

TEST(SseStreamBodyTest, PushMultipleChunks_ReadDrainsAll) {
  SseStreamBody body;
  body.Push("data: chunk1\n\n");
  body.Push("data: chunk2\n\n");
  body.Finish();

  // Use a large buffer to drain everything in one read
  char buffer[1024] = {};
  oatpp::async::Action action;
  auto bytes_read = body.read(buffer, sizeof(buffer), action);

  std::string result(buffer, bytes_read);
  EXPECT_EQ(result, "data: chunk1\n\ndata: chunk2\n\n");
}

TEST(SseStreamBodyTest, ReadReturnsZeroWhenFinishedAndEmpty) {
  SseStreamBody body;
  body.Finish();

  char buffer[64] = {};
  oatpp::async::Action action;
  auto bytes_read = body.read(buffer, sizeof(buffer), action);

  EXPECT_EQ(bytes_read, 0);
}

TEST(SseStreamBodyTest, PartialRead_SmallBuffer) {
  SseStreamBody body;
  std::string chunk = "data: hello world\n\n";
  body.Push(chunk);
  body.Finish();

  // Read with a buffer smaller than the chunk
  char buffer[10] = {};
  oatpp::async::Action action;
  auto bytes1 = body.read(buffer, sizeof(buffer), action);
  ASSERT_EQ(bytes1, 10);
  std::string part1(buffer, bytes1);
  EXPECT_EQ(part1, "data: hell");

  // Read the rest
  char buffer2[64] = {};
  auto bytes2 = body.read(buffer2, sizeof(buffer2), action);
  std::string part2(buffer2, bytes2);
  EXPECT_EQ(part2, "o world\n\n");
}

// ========================================================================
// SseStreamBody::Finish
// ========================================================================

TEST(SseStreamBodyTest, FinishAfterPush_DataStillReadable) {
  SseStreamBody body;
  body.Push("data: important\n\n");
  body.Finish();

  char buffer[128] = {};
  oatpp::async::Action action;
  auto bytes_read = body.read(buffer, sizeof(buffer), action);
  std::string result(buffer, bytes_read);
  EXPECT_EQ(result, "data: important\n\n");

  // Subsequent read returns EOF
  auto bytes_eof = body.read(buffer, sizeof(buffer), action);
  EXPECT_EQ(bytes_eof, 0);
}

// ========================================================================
// SseStreamBody::declareHeaders
// ========================================================================

TEST(SseStreamBodyTest, DeclareHeaders_SetsCorrectValues) {
  SseStreamBody body;

  // oatpp Headers is a multimap-like container
  oatpp::web::protocol::http::Headers headers;
  body.declareHeaders(headers);

  auto content_type = headers.get("Content-Type");
  ASSERT_TRUE(content_type);
  EXPECT_EQ(std::string(content_type->c_str()), std::string("text/event-stream"));

  auto cache_control = headers.get("Cache-Control");
  ASSERT_TRUE(cache_control);
  EXPECT_EQ(std::string(cache_control->c_str()), std::string("no-cache"));

  auto connection = headers.get("Connection");
  ASSERT_TRUE(connection);
  EXPECT_EQ(std::string(connection->c_str()), std::string("keep-alive"));
}

// ========================================================================
// SseStreamBody metadata
// ========================================================================

TEST(SseStreamBodyTest, GetKnownSize_ReturnsNegativeOne) {
  SseStreamBody body;
  EXPECT_EQ(body.getKnownSize(), -1);
}

TEST(SseStreamBodyTest, GetKnownData_ReturnsNullptr) {
  SseStreamBody body;
  EXPECT_EQ(body.getKnownData(), nullptr);
}

TEST(SseStreamBodyTest, ClosingBeforeEofCancelsRunningRequest) {
  auto request = std::make_shared<Request>();
  ASSERT_TRUE(request->TryBegin());
  auto body = std::make_unique<SseStreamBody>();
  auto stream = body->Stream();
  stream->BindRequest(request);

  body.reset();

  EXPECT_TRUE(stream->IsDisconnected());
  EXPECT_TRUE(request->IsCancellationRequested());
  stream->Push("data: discarded\n\n");
  stream->Finish();
}

TEST(SseStreamBodyTest, ClosingBeforeRequestStartsPreventsProducerFromStarting) {
  auto request = std::make_shared<Request>();
  auto body = std::make_unique<SseStreamBody>();
  auto stream = body->Stream();
  stream->BindRequest(request);

  body.reset();

  EXPECT_TRUE(stream->IsDisconnected());
  ASSERT_TRUE(request->TryBegin());
  EXPECT_TRUE(request->IsCancellationRequested());
}

TEST(SseStreamBodyTest, NormalEofDoesNotCancelRunningRequest) {
  auto request = std::make_shared<Request>();
  ASSERT_TRUE(request->TryBegin());
  auto body = std::make_unique<SseStreamBody>();
  body->Stream()->BindRequest(request);
  body->Push("data: [DONE]\n\n");
  body->Finish();

  char buffer[64];
  oatpp::async::Action action;
  EXPECT_GT(body->read(buffer, sizeof(buffer), action), 0);
  EXPECT_EQ(body->read(buffer, sizeof(buffer), action), 0);
  body.reset();

  EXPECT_FALSE(request->IsCancellationRequested());
  EXPECT_TRUE(request->TryComplete());
  request->PublishCompletion();
  EXPECT_TRUE(request->IsCompleted());
}

TEST(SseStreamBodyTest, ClosingAfterInferenceCompletedLeavesRequestReusable) {
  auto request = std::make_shared<Request>();
  ASSERT_TRUE(request->TryBegin());
  ASSERT_TRUE(request->TryComplete());
  request->PublishCompletion();
  auto body = std::make_unique<SseStreamBody>();
  body->Stream()->BindRequest(request);

  body.reset();

  EXPECT_FALSE(request->IsCancellationRequested());
  ASSERT_TRUE(request->TryBegin());
  EXPECT_FALSE(request->IsCancellationRequested());
}

TEST(SseStreamBodyTest, IdleReadSendsKeepAliveBeforeInferenceProducesData) {
  SseStreamBody body;
  char buffer[64];
  oatpp::async::Action action;
  auto start = std::chrono::steady_clock::now();

  const auto bytes = body.read(buffer, sizeof(buffer), action);

  EXPECT_EQ(std::string(buffer, bytes), ": keep-alive\n\n");
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  body.Finish();
}

// ========================================================================
// SseStreamBody — threaded Push + read
// ========================================================================

TEST(SseStreamBodyTest, ConcurrentPushAndRead) {
  SseStreamBody body;
  constexpr int kChunkCount = 50;
  std::string total_read;

  // Producer thread
  std::thread producer([&body] {
    for (int i = 0; i < kChunkCount; ++i) {
      body.Push("data: " + std::to_string(i) + "\n\n");
    }

    body.Finish();
  });

  // Consumer: read until EOF
  char buffer[256];
  oatpp::async::Action action;
  while (true) {
    auto n = body.read(buffer, sizeof(buffer), action);
    if (n == 0) {
      break;
    }

    total_read.append(buffer, n);
  }

  producer.join();

  // Verify all chunks were received
  for (int i = 0; i < kChunkCount; ++i) {
    std::string expected = "data: " + std::to_string(i) + "\n\n";
    EXPECT_NE(total_read.find(expected), std::string::npos)
        << "Missing chunk " << i;
  }
}

TEST(StreamingThreadTrackerTest, QuickWorkersDoNotAccumulate) {
  StreamingThreadTracker tracker;
  std::atomic<int> completed{0};
  constexpr int kWorkerCount = 100;
  for (int i = 0; i < kWorkerCount; ++i) {
    tracker.Start([&completed] { completed.fetch_add(1, std::memory_order_release); });
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((completed.load(std::memory_order_acquire) != kWorkerCount || tracker.TrackedCount() != 0) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  EXPECT_EQ(completed.load(std::memory_order_acquire), kWorkerCount);
  EXPECT_EQ(tracker.TrackedCount(), 0u);
  tracker.JoinAll();
}

TEST(StreamingThreadTrackerTest, ShutdownWaitsForWorkerCaptureCleanup) {
  struct BlockingCleanup {
    std::promise<void>& entered;
    std::shared_future<void> release;

    ~BlockingCleanup() {
      entered.set_value();
      release.wait();
    }
  };

  StreamingThreadTracker tracker;
  std::promise<void> cleanup_entered;
  std::promise<void> allow_cleanup;
  auto cleanup_future = cleanup_entered.get_future();
  auto release = allow_cleanup.get_future().share();
  tracker.Start([cleanup = std::make_unique<BlockingCleanup>(cleanup_entered, release)] {});

  const auto cleanup_started = cleanup_future.wait_for(std::chrono::seconds(5));
  std::promise<void> shutdown_finished;
  auto shutdown_future = shutdown_finished.get_future();
  std::thread shutdown([&] {
    tracker.JoinAll();
    shutdown_finished.set_value();
  });

  EXPECT_EQ(cleanup_started, std::future_status::ready);
  EXPECT_EQ(shutdown_future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  allow_cleanup.set_value();
  EXPECT_EQ(shutdown_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  shutdown.join();
}

// ========================================================================
// GenerateCompletionId — fixed-width IDs
// ========================================================================

TEST(GenerateCompletionIdTest, ProducesFixedWidthIdsAcrossManyCalls) {
  // Two 32-bit values rendered as zero-padded hex = 16 hex chars.
  const std::string prefix = "chatcmpl";
  const size_t expected_len = prefix.size() + 1 /* '-' */ + 16;

  std::unordered_set<std::string> ids;
  for (int i = 0; i < 1000; ++i) {
    std::string id = GenerateCompletionId(prefix);
    EXPECT_EQ(id.size(), expected_len) << "id=" << id;
    EXPECT_EQ(id.substr(0, prefix.size() + 1), prefix + "-");
    ids.insert(id);
  }

  // Sanity: IDs should be (overwhelmingly) unique.
  EXPECT_GT(ids.size(), 990u);
}

TEST(GenerateCompletionIdTest, ZeroPadsLowEntropyValues) {
  // Generate many IDs and confirm none of them have a hex tail shorter than 16
  // chars (the original bug: small random values produced shorter IDs).
  const std::string prefix = "x";
  for (int i = 0; i < 5000; ++i) {
    std::string id = GenerateCompletionId(prefix);
    auto dash = id.find('-');
    ASSERT_NE(dash, std::string::npos);
    EXPECT_EQ(id.size() - dash - 1, 16u) << "id=" << id;
  }
}

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
