// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Tests for the C++ Item hierarchy in items/item.h.
// Pure unit tests — no C API, no model, no network.
//
#include "items/audio_item.h"
#include "items/bytes_item.h"
#include "items/image_item.h"
#include "items/item_queue.h"
#include "items/message_item.h"
#include "items/speech_result_item.h"
#include "items/speech_segment_item.h"
#include "items/tensor_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "inferencing/session/session.h"
#include "exception.h"

#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fl;

// ========================================================================
// Item::Create factory — every derived type
// ========================================================================

TEST(ItemCreateTest, TextItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_TEXT);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_TEXT);

  auto* text = dynamic_cast<TextItem*>(item.get());
  ASSERT_NE(text, nullptr);
  EXPECT_TRUE(text->text.empty());
}

TEST(ItemCreateTest, MessageItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_MESSAGE);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_MESSAGE);

  auto* msg = dynamic_cast<MessageItem*>(item.get());
  ASSERT_NE(msg, nullptr);
  EXPECT_TRUE(msg->role == FOUNDRY_LOCAL_ROLE_NONE);
  EXPECT_TRUE(msg->content.empty());
}

TEST(ItemCreateTest, ToolCallItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL);

  auto* tc = dynamic_cast<ToolCallItem*>(item.get());
  ASSERT_NE(tc, nullptr);
  EXPECT_TRUE(tc->call_id.empty());
  EXPECT_TRUE(tc->name.empty());
  EXPECT_TRUE(tc->arguments.empty());
}

TEST(ItemCreateTest, ToolResultItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_TOOL_RESULT);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_TOOL_RESULT);

  auto* tr = dynamic_cast<ToolResultItem*>(item.get());
  ASSERT_NE(tr, nullptr);
  EXPECT_TRUE(tr->call_id.empty());
  EXPECT_TRUE(tr->result.empty());
}

TEST(ItemCreateTest, ImageItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_IMAGE);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_IMAGE);

  auto* img = dynamic_cast<ImageItem*>(item.get());
  ASSERT_NE(img, nullptr);
  EXPECT_EQ(img->data, nullptr);
  EXPECT_EQ(img->data_size, 0u);
  EXPECT_TRUE(img->format.empty());
  EXPECT_TRUE(img->uri.empty());
}

TEST(ItemCreateTest, AudioItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_AUDIO);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_AUDIO);

  auto* audio = dynamic_cast<AudioItem*>(item.get());
  ASSERT_NE(audio, nullptr);
  EXPECT_EQ(audio->data, nullptr);
  EXPECT_EQ(audio->data_size, 0u);
}

TEST(ItemCreateTest, TensorItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_TENSOR);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_TENSOR);

  auto* tensor = dynamic_cast<TensorItem*>(item.get());
  ASSERT_NE(tensor, nullptr);
  EXPECT_TRUE(tensor->data_type == FOUNDRY_LOCAL_TENSOR_UNDEFINED);
  EXPECT_EQ(tensor->data, nullptr);
  EXPECT_TRUE(tensor->shape.empty());
}

TEST(ItemCreateTest, BytesItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_BYTES);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_BYTES);

  auto* bytes = dynamic_cast<BytesItem*>(item.get());
  ASSERT_NE(bytes, nullptr);
  EXPECT_TRUE(bytes->item_type == FOUNDRY_LOCAL_ITEM_UNKNOWN);
  EXPECT_EQ(bytes->data, nullptr);
  EXPECT_EQ(bytes->data_size, 0u);
}

TEST(ItemCreateTest, QueueItem) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_QUEUE);
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(item->type == FOUNDRY_LOCAL_ITEM_QUEUE);

  auto* q = dynamic_cast<ItemQueue*>(item.get());
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->Size(), 0u);
  EXPECT_FALSE(q->IsFinished());
}

TEST(ItemCreateTest, UnknownTypeReturnsNullptr) {
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_UNKNOWN);
  EXPECT_EQ(item, nullptr);
}

TEST(ItemCreateTest, SpeechSegmentNotCreatable) {
  // SPEECH_SEGMENT is output-only; the factory does not produce one.
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT);
  EXPECT_EQ(item, nullptr);
}

TEST(ItemCreateTest, SpeechResultNotCreatable) {
  // SPEECH_RESULT is output-only; the factory does not produce one.
  auto item = Item::Create(FOUNDRY_LOCAL_ITEM_SPEECH_RESULT);
  EXPECT_EQ(item, nullptr);
}

TEST(ItemCreateTest, InvalidEnumValueReturnsNullptr) {
  auto item = Item::Create(static_cast<flItemType>(9999));
  EXPECT_EQ(item, nullptr);
}

// ========================================================================
// Derived type construction with values
// ========================================================================

TEST(TextItemTest, ConstructWithValue) {
  TextItem item("hello world");
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(item.text, "hello world");
}

TEST(MessageItemTest, ConstructWithAllFields) {
  MessageItem item(FOUNDRY_LOCAL_ROLE_USER, "What is 2+2?", "Alice");
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_TRUE(item.role == FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(item.GetSimpleText(), "What is 2+2?");
  EXPECT_EQ(item.name, "Alice");
}

TEST(MessageItemTest, ConstructWithDefaults) {
  MessageItem item;
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_TRUE(item.role == FOUNDRY_LOCAL_ROLE_NONE);
  EXPECT_TRUE(item.content.empty());
  EXPECT_TRUE(item.name.empty());
}

TEST(ToolCallItemTest, ConstructWithValues) {
  ToolCallItem item("call_123", "get_weather", R"({"city":"Seattle"})");
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_TOOL_CALL);
  EXPECT_EQ(item.call_id, "call_123");
  EXPECT_EQ(item.name, "get_weather");
  EXPECT_EQ(item.arguments, R"({"city":"Seattle"})");
}

TEST(ToolResultItemTest, ConstructWithValues) {
  ToolResultItem item("call_123", "72 degrees");
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_TOOL_RESULT);
  EXPECT_EQ(item.call_id, "call_123");
  EXPECT_EQ(item.result, "72 degrees");
}

// ========================================================================
// Speech items (output-only)
// ========================================================================

TEST(SpeechSegmentItemTest, DefaultsAreEmpty) {
  SpeechSegmentItem item;
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT);
  EXPECT_EQ(item.kind, FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE);
  EXPECT_TRUE(item.text.empty());
  EXPECT_FALSE(item.start_time_ms.has_value());
  EXPECT_FALSE(item.end_time_ms.has_value());
  EXPECT_FALSE(item.utterance_start);
  EXPECT_TRUE(item.words.empty());
  EXPECT_TRUE(item.language.empty());
}

TEST(SpeechSegmentItemTest, ConstructWithKindAndText) {
  SpeechSegmentItem item(FOUNDRY_LOCAL_SPEECH_SEGMENT_PARTIAL, "hello");
  EXPECT_EQ(item.kind, FOUNDRY_LOCAL_SPEECH_SEGMENT_PARTIAL);
  EXPECT_EQ(item.text, "hello");
}

TEST(SpeechSegmentItemTest, GetApiDataMapsOptionalsToSentinel) {
  SpeechSegmentItem item(FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL, "the cat sat");
  item.utterance_start = true;
  item.start_time_ms = 100;
  item.end_time_ms = 1500;
  item.language = "en";
  item.words.push_back({"the", 100, 200, 0.95f, ""});
  item.words.push_back({"cat", std::nullopt, std::nullopt, std::nullopt, "spk_1"});
  item.Finalize();

  flSpeechSegmentData out{};
  out.version = FOUNDRY_LOCAL_API_VERSION;
  item.GetApiData(out);

  EXPECT_EQ(out.kind, FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL);
  EXPECT_STREQ(out.text, "the cat sat");
  EXPECT_EQ(out.start_time_ms, 100);
  EXPECT_EQ(out.end_time_ms, 1500);
  EXPECT_TRUE(out.utterance_start);
  EXPECT_STREQ(out.language, "en");
  ASSERT_EQ(out.words_count, 2u);

  EXPECT_STREQ(out.words[0].text, "the");
  EXPECT_EQ(out.words[0].start_time_ms, 100);
  EXPECT_EQ(out.words[0].end_time_ms, 200);
  EXPECT_FLOAT_EQ(out.words[0].confidence, 0.95f);
  EXPECT_EQ(out.words[0].speaker_id, nullptr);

  EXPECT_STREQ(out.words[1].text, "cat");
  EXPECT_EQ(out.words[1].start_time_ms, FOUNDRY_LOCAL_DURATION_UNSET);
  EXPECT_EQ(out.words[1].end_time_ms, FOUNDRY_LOCAL_DURATION_UNSET);
  EXPECT_EQ(out.words[1].confidence, FOUNDRY_LOCAL_CONFIDENCE_UNSET);
  EXPECT_STREQ(out.words[1].speaker_id, "spk_1");
}

TEST(SpeechSegmentItemTest, GetApiDataEmptyOptionalsBecomeSentinel) {
  SpeechSegmentItem item;
  item.Finalize();
  flSpeechSegmentData out{};
  out.version = FOUNDRY_LOCAL_API_VERSION;
  item.GetApiData(out);

  EXPECT_EQ(out.start_time_ms, FOUNDRY_LOCAL_DURATION_UNSET);
  EXPECT_EQ(out.end_time_ms, FOUNDRY_LOCAL_DURATION_UNSET);
  EXPECT_EQ(out.language, nullptr);
  EXPECT_EQ(out.words, nullptr);
  EXPECT_EQ(out.words_count, 0u);
}

TEST(SpeechResultItemTest, DefaultsAreEmpty) {
  SpeechResultItem item;
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_SPEECH_RESULT);
  EXPECT_TRUE(item.text.empty());
  EXPECT_TRUE(item.language.empty());
  EXPECT_FALSE(item.duration_ms.has_value());
  EXPECT_TRUE(item.segments.empty());
}

TEST(SpeechResultItemTest, GetApiDataExposesSegmentsAsItemPointers) {
  SpeechResultItem result("the cat sat");
  result.language = "en";
  result.duration_ms = 1500;
  result.segments.push_back(std::make_unique<SpeechSegmentItem>(FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL, "the cat"));
  result.segments.push_back(std::make_unique<SpeechSegmentItem>(FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL, "sat"));
  result.Finalize();

  flSpeechResultData out{};
  out.version = FOUNDRY_LOCAL_API_VERSION;
  result.GetApiData(out);

  EXPECT_STREQ(out.text, "the cat sat");
  EXPECT_STREQ(out.language, "en");
  EXPECT_EQ(out.duration_ms, 1500);
  ASSERT_EQ(out.segments_count, 2u);

  // Each segment pointer should resolve back to a SPEECH_SEGMENT item.
  for (size_t i = 0; i < out.segments_count; ++i) {
    ASSERT_NE(out.segments[i], nullptr);
    EXPECT_TRUE(reinterpret_cast<const Item*>(out.segments[i])->type == FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT);
  }
}

// Wrapper translation: sentinel int64 ↔ std::optional<int64>, NULL ↔ std::optional<string_view>,
// and that segments are exposed as a vector<Item> of SPEECH_SEGMENT items.
TEST(SpeechWrapperTest, ReadsThroughPublicCppApi) {
  SpeechSegmentItem seg(FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL, "the cat sat");
  seg.start_time_ms = 100;
  seg.utterance_start = true;
  // end_time_ms intentionally left unset.
  seg.words.push_back({"the", 100, 200, 0.95f, ""});
  seg.words.push_back({"cat", std::nullopt, std::nullopt, std::nullopt, ""});
  seg.Finalize();

  const fl::SpeechSegmentItem& cseg = seg;
  foundry_local::Item view(*cseg.AsApiType());
  auto content = view.GetSpeechSegment();
  EXPECT_EQ(content.kind, FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL);
  EXPECT_EQ(content.text, "the cat sat");
  ASSERT_TRUE(content.start_time_ms.has_value());
  EXPECT_EQ(*content.start_time_ms, 100);
  EXPECT_FALSE(content.end_time_ms.has_value());
  EXPECT_TRUE(content.utterance_start);
  EXPECT_FALSE(content.language.has_value());
  ASSERT_EQ(content.words.size(), 2u);

  EXPECT_EQ(content.words[0].text, "the");
  ASSERT_TRUE(content.words[0].confidence.has_value());
  EXPECT_FLOAT_EQ(*content.words[0].confidence, 0.95f);
  EXPECT_FALSE(content.words[0].speaker_id.has_value());

  EXPECT_FALSE(content.words[1].start_time_ms.has_value());
  EXPECT_FALSE(content.words[1].confidence.has_value());
}

TEST(SpeechWrapperTest, ResultExposesSegmentsAsItemViews) {
  SpeechResultItem result("hi");
  result.duration_ms = 1500;
  result.segments.push_back(std::make_unique<SpeechSegmentItem>(FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL, "hi"));
  result.Finalize();

  const fl::SpeechResultItem& cresult = result;
  foundry_local::Item view(*cresult.AsApiType());
  auto content = view.GetSpeechResult();
  EXPECT_EQ(content.text, "hi");
  ASSERT_TRUE(content.duration_ms.has_value());
  EXPECT_EQ(*content.duration_ms, 1500);
  ASSERT_EQ(content.segments.size(), 1u);
  EXPECT_EQ(content.segments[0].GetType(), FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT);
  EXPECT_EQ(content.segments[0].GetSpeechSegment().text, "hi");
}

TEST(JsonItemTest, OpenAIJsonTextItem) {
  TextItem item(R"({"model":"gpt-4","input":"hello"})", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(item.text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  EXPECT_EQ(item.text, R"({"model":"gpt-4","input":"hello"})");
}

TEST(JsonItemTest, OpenAIJsonTextItemDefaultEmpty) {
  TextItem item("", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  EXPECT_TRUE(item.type == FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(item.text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  EXPECT_TRUE(item.text.empty());
}

// ========================================================================
// ItemQueue lifecycle
// ========================================================================

TEST(ItemQueueTest, EmptyQueueReturnsNullptrOnPop) {
  ItemQueue queue;
  EXPECT_EQ(queue.Size(), 0u);
  EXPECT_EQ(queue.TryPop(), nullptr);
}

TEST(ItemQueueTest, PushPopSingle) {
  ItemQueue queue;
  queue.Push(std::make_unique<TextItem>("first"));
  EXPECT_EQ(queue.Size(), 1u);

  auto popped = queue.TryPop();
  ASSERT_NE(popped, nullptr);
  EXPECT_TRUE(popped->type == FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<TextItem*>(popped.get())->text, "first");
  EXPECT_EQ(queue.Size(), 0u);
}

TEST(ItemQueueTest, PushPopMultipleFIFO) {
  ItemQueue queue;
  queue.Push(std::make_unique<TextItem>("a"));
  queue.Push(std::make_unique<TextItem>("b"));
  queue.Push(std::make_unique<TextItem>("c"));
  EXPECT_EQ(queue.Size(), 3u);

  auto item1 = queue.TryPop();
  auto item2 = queue.TryPop();
  auto item3 = queue.TryPop();

  EXPECT_EQ(static_cast<TextItem*>(item1.get())->text, "a");
  EXPECT_EQ(static_cast<TextItem*>(item2.get())->text, "b");
  EXPECT_EQ(static_cast<TextItem*>(item3.get())->text, "c");
  EXPECT_EQ(queue.Size(), 0u);
}

TEST(ItemQueueTest, MarkFinishedLifecycle) {
  ItemQueue queue;
  EXPECT_FALSE(queue.IsFinished());

  queue.Push(std::make_unique<TextItem>("data"));
  queue.MarkFinished();
  EXPECT_TRUE(queue.IsFinished());

  // Can still pop items after marking finished
  auto item = queue.TryPop();
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(static_cast<TextItem*>(item.get())->text, "data");

  // Queue is empty AND finished
  EXPECT_TRUE(queue.IsFinished());
  EXPECT_EQ(queue.TryPop(), nullptr);
}

TEST(ItemQueueTest, MixedItemTypes) {
  ItemQueue queue;
  queue.Push(std::make_unique<TextItem>("hello"));
  queue.Push(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "hi"));
  queue.Push(std::make_unique<ToolCallItem>("call_1", "fn", "{}"));

  EXPECT_EQ(queue.Size(), 3u);

  auto item1 = queue.TryPop();
  EXPECT_TRUE(item1->type == FOUNDRY_LOCAL_ITEM_TEXT);

  auto item2 = queue.TryPop();
  EXPECT_TRUE(item2->type == FOUNDRY_LOCAL_ITEM_MESSAGE);

  auto item3 = queue.TryPop();
  EXPECT_TRUE(item3->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL);
}

// ========================================================================
// ItemQueue — concurrent access
// ========================================================================

TEST(ItemQueueTest, ConcurrentPushAndPop) {
  ItemQueue queue;
  constexpr int kItemCount = 500;
  std::atomic<int> popped_count{0};

  // Producer thread: push kItemCount items
  std::thread producer([&queue] {
    for (int i = 0; i < kItemCount; ++i) {
      queue.Push(std::make_unique<TextItem>(std::to_string(i)));
    }

    queue.MarkFinished();
  });

  // Consumer thread: pop until finished and empty
  std::thread consumer([&queue, &popped_count] {
    while (true) {
      auto item = queue.TryPop();
      if (item) {
        popped_count.fetch_add(1);
      } else if (queue.IsFinished()) {
        // Drain any remaining items after finish signal
        while (auto remaining = queue.TryPop()) {
          popped_count.fetch_add(1);
        }

        break;
      }

      // Yield briefly if nothing to pop yet
      std::this_thread::yield();
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(popped_count.load(), kItemCount);
  EXPECT_EQ(queue.Size(), 0u);
  EXPECT_TRUE(queue.IsFinished());
}

TEST(ItemQueueTest, MultipleProducersSingleConsumer) {
  ItemQueue queue;
  constexpr int kItemsPerProducer = 200;
  constexpr int kProducerCount = 3;
  constexpr int kTotalItems = kItemsPerProducer * kProducerCount;
  std::atomic<int> popped_count{0};
  std::atomic<int> producers_done{0};

  // Multiple producer threads
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducerCount; ++p) {
    producers.emplace_back([&queue, &producers_done, p] {
      for (int i = 0; i < kItemsPerProducer; ++i) {
        queue.Push(std::make_unique<TextItem>(
            "p" + std::to_string(p) + "_" + std::to_string(i)));
      }

      if (producers_done.fetch_add(1) + 1 == kProducerCount) {
        queue.MarkFinished();
      }
    });
  }

  // Single consumer
  std::thread consumer([&queue, &popped_count] {
    while (true) {
      auto item = queue.TryPop();
      if (item) {
        popped_count.fetch_add(1);
      } else if (queue.IsFinished()) {
        while (auto remaining = queue.TryPop()) {
          popped_count.fetch_add(1);
        }

        break;
      }

      std::this_thread::yield();
    }
  });

  for (auto& t : producers) {
    t.join();
  }

  consumer.join();

  EXPECT_EQ(popped_count.load(), kTotalItems);
  EXPECT_EQ(queue.Size(), 0u);
}

// ========================================================================
// Per-type item deleter callbacks (embedded in data structs)
// ========================================================================

TEST(ItemDeleterTest, TensorDeleterCalledOnDestruction) {
  bool deleter_called = false;
  float buf[] = {1.0f};
  int64_t shape[] = {1};

  auto callback = [](const flTensorData* td, void* user_data) {
    EXPECT_NE(td->mutable_data, nullptr);
    *static_cast<bool*>(user_data) = true;
  };

  {
    auto item = std::make_unique<TensorItem>();
    flTensorData td{};
    td.version = FOUNDRY_LOCAL_API_VERSION;
    td.data_type = FOUNDRY_LOCAL_TENSOR_FLOAT;
    td.data = buf;
    td.mutable_data = buf;
    td.shape = shape;
    td.rank = 1;
    td.deleter = callback;
    td.deleter_user_data = &deleter_called;
    item->SetTensorData(td);
  }  // destructor fires here

  EXPECT_TRUE(deleter_called);
}

TEST(ItemDeleterTest, AudioDeleterReceivesUserData) {
  int marker = 0;
  uint8_t samples[] = {0};

  auto callback = [](const flAudioData* ad, void* user_data) {
    EXPECT_NE(ad->mutable_data, nullptr);
    *static_cast<int*>(user_data) = 42;
  };

  {
    auto item = std::make_unique<AudioItem>();
    flAudioData ad{};
    ad.version = FOUNDRY_LOCAL_API_VERSION;
    ad.data = samples;
    ad.mutable_data = samples;
    ad.data_size = sizeof(samples);
    ad.format = "raw";
    ad.uri = nullptr;
    ad.deleter = callback;
    ad.deleter_user_data = &marker;
    item->SetAudioData(ad);
  }

  EXPECT_EQ(marker, 42);
}

TEST(ItemDeleterTest, NoDeleterDoesNotCrash) {
  // Default-constructed items have no deleter — destruction should not crash.
  {
    auto item = std::make_unique<TextItem>("safe");
  }
  {
    auto item = std::make_unique<AudioItem>();
  }
  {
    auto item = std::make_unique<TensorItem>();
  }
}

// ========================================================================
// Item metadata — access via GetMetadata()
// ========================================================================

TEST(ItemMetadataTest, MetadataIsNullByDefault) {
  TextItem item("test");
  const auto& citem = item;
  EXPECT_EQ(citem.GetMetadata(), nullptr);
}

TEST(ItemMetadataTest, MutableMetadataCreatesOnDemand) {
  TextItem item("test");
  KeyValuePairs& meta = item.GetMetadata();
  meta["key1"] = "value1";
  meta["key2"] = "value2";

  EXPECT_STREQ(meta.Find("key1"), "value1");
  EXPECT_STREQ(meta.Find("key2"), "value2");
  EXPECT_EQ(meta.Find("nonexistent"), nullptr);

  // Const accessor returns same data
  const auto* cmeta = const_cast<const TextItem&>(item).GetMetadata();
  ASSERT_NE(cmeta, nullptr);
  EXPECT_STREQ(cmeta->Find("key1"), "value1");
}

// ========================================================================
// Request — AddOwnedItem / AddBorrowedItem
// Verifies behavior through public API. owned_items is private.
// ========================================================================

TEST(RequestTest, AddOwnedItemAppearsInItems) {
  Request req;
  auto item = std::make_unique<TextItem>("owned");
  TextItem* raw = item.get();

  req.AddOwnedItem(std::move(item));

  EXPECT_EQ(req.items.size(), 1u);
  EXPECT_EQ(req.items[0], raw);
}

TEST(RequestTest, AddBorrowedItemAppearsInItems) {
  Request req;
  TextItem item("borrowed");

  req.AddBorrowedItem(&item);

  EXPECT_EQ(req.items.size(), 1u);
  EXPECT_EQ(req.items[0], &item);
}

TEST(RequestTest, MixedOwnedAndBorrowedItems) {
  Request req;
  TextItem borrowed("borrowed");
  req.AddBorrowedItem(&borrowed);

  auto owned = std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "hello");
  req.AddOwnedItem(std::move(owned));

  EXPECT_EQ(req.items.size(), 2u);
  EXPECT_TRUE(req.items[0]->type == FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_TRUE(req.items[1]->type == FOUNDRY_LOCAL_ITEM_MESSAGE);
}

TEST(RequestTest, CancellationFlag) {
  Request req;
  EXPECT_FALSE(req.canceled);

  req.canceled = true;
  EXPECT_TRUE(req.canceled);
}

// ========================================================================
// Response — basic structure
// ========================================================================

TEST(ResponseTest, DefaultValues) {
  Response resp;
  EXPECT_TRUE(resp.items.empty());
  EXPECT_TRUE(resp.finish_reason == FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_EQ(resp.usage.prompt_tokens, static_cast<int64_t>(0));
  EXPECT_EQ(resp.usage.completion_tokens, static_cast<int64_t>(0));
  EXPECT_EQ(resp.usage.total_tokens, static_cast<int64_t>(0));
}

TEST(ResponseTest, AddItems) {
  Response resp;
  resp.items.push_back(std::make_unique<TextItem>("response text"));
  resp.items.push_back(std::make_unique<ToolCallItem>("call_1", "fn", "{}"));

  EXPECT_EQ(resp.items.size(), 2u);
  EXPECT_TRUE(resp.items[0]->type == FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_TRUE(resp.items[1]->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL);
}

// ========================================================================
// ImageItem — SetImageData + GetApiData
// ========================================================================

TEST(ImageItemTest, SetImageData_FromRawBytes) {
  ImageItem item;
  uint8_t pixels[] = {0xFF, 0x00, 0xAA};
  flImageData in = {};
  in.version = FOUNDRY_LOCAL_API_VERSION;
  in.data = pixels;
  in.data_size = sizeof(pixels);
  in.format = "png";
  in.uri = nullptr;

  item.SetImageData(in);

  EXPECT_EQ(item.data, pixels);
  EXPECT_EQ(item.data_size, sizeof(pixels));
  EXPECT_EQ(item.format, "png");
  EXPECT_TRUE(item.uri.empty());
}

TEST(ImageItemTest, SetImageData_FromUri) {
  ImageItem item;
  flImageData in = {};
  in.version = FOUNDRY_LOCAL_API_VERSION;
  in.data = nullptr;
  in.data_size = 0;
  in.format = "jpeg";
  in.uri = "https://example.com/img.jpg";

  item.SetImageData(in);

  EXPECT_EQ(item.data, nullptr);
  EXPECT_EQ(item.data_size, 0u);
  EXPECT_EQ(item.format, "jpeg");
  EXPECT_EQ(item.uri, "https://example.com/img.jpg");
}

TEST(ImageItemTest, GetApiData_RoundTrip) {
  uint8_t pixels[] = {1, 2, 3};
  ImageItem item(pixels, sizeof(pixels), "bmp");

  flImageData out = {};
  item.GetApiData(out);

  EXPECT_EQ(out.version, FOUNDRY_LOCAL_API_VERSION);
  EXPECT_EQ(out.data, pixels);
  EXPECT_EQ(out.data_size, sizeof(pixels));
  EXPECT_STREQ(out.format, "bmp");
  EXPECT_EQ(out.uri, nullptr);  // empty uri → nullptr
}

TEST(ImageItemTest, GetApiData_WithUri) {
  ImageItem item("https://example.com/pic.png", "png");

  flImageData out = {};
  item.GetApiData(out);

  EXPECT_EQ(out.data, nullptr);
  EXPECT_EQ(out.data_size, 0u);
  EXPECT_STREQ(out.format, "png");
  ASSERT_NE(out.uri, nullptr);
  EXPECT_STREQ(out.uri, "https://example.com/pic.png");
}

TEST(ImageItemTest, ReadBytes_FromInMemoryBuffer) {
  const std::uint8_t bytes[] = {0xde, 0xad, 0xbe, 0xef};
  ImageItem item(bytes, sizeof(bytes), "image/png");

  auto out = item.ReadBytes();

  ASSERT_EQ(out.size(), sizeof(bytes));
  EXPECT_EQ(out[0], 0xde);
  EXPECT_EQ(out[3], 0xef);
}

TEST(ImageItemTest, ReadBytes_FromFileUri) {
  // Write a temp file and read it back via ReadBytes().
  auto tmp = std::filesystem::temp_directory_path() /
             ("fl_image_test_" + std::to_string(static_cast<long long>(std::rand())) + ".bin");

  const std::uint8_t bytes[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  }

  ImageItem item(tmp.string(), "image/png");
  auto data = item.ReadBytes();

  std::filesystem::remove(tmp);

  ASSERT_EQ(data.size(), sizeof(bytes));
  EXPECT_EQ(data[0], 0x01);
  EXPECT_EQ(data[4], 0x05);
}

TEST(ImageItemTest, ReadBytes_MissingUriThrows) {
  ImageItem item(std::string("/nonexistent/path/foundry_local_should_never_exist.bin"), "image/png");
  EXPECT_THROW(item.ReadBytes(), fl::Exception);
}

TEST(ImageItemTest, ReadBytes_NoBytesNoUriThrows) {
  ImageItem item;  // default-constructed: no data, no uri
  EXPECT_THROW(item.ReadBytes(), fl::Exception);
}

// ========================================================================
// AudioItem — SetAudioData + GetApiData
// ========================================================================

TEST(AudioItemTest, SetAudioData_FromRawBytes) {
  AudioItem item;
  uint8_t samples[] = {0x01, 0x02};
  flAudioData in = {};
  in.version = FOUNDRY_LOCAL_API_VERSION;
  in.data = samples;
  in.data_size = sizeof(samples);
  in.format = "wav";
  in.uri = nullptr;

  item.SetAudioData(in);

  EXPECT_EQ(item.data, samples);
  EXPECT_EQ(item.data_size, sizeof(samples));
  EXPECT_EQ(item.format, "wav");
  EXPECT_TRUE(item.uri.empty());
}

TEST(AudioItemTest, GetApiData_RoundTrip) {
  uint8_t samples[] = {0xAA, 0xBB};
  AudioItem item(samples, sizeof(samples), "mp3");

  flAudioData out = {};
  item.GetApiData(out);

  EXPECT_EQ(out.version, FOUNDRY_LOCAL_API_VERSION);
  EXPECT_EQ(out.data, samples);
  EXPECT_EQ(out.data_size, sizeof(samples));
  EXPECT_STREQ(out.format, "mp3");
  EXPECT_EQ(out.uri, nullptr);  // empty uri → nullptr
}

TEST(AudioItemTest, GetApiData_WithUri) {
  AudioItem item("https://example.com/audio.wav", "wav");

  flAudioData out = {};
  item.GetApiData(out);

  EXPECT_EQ(out.data, nullptr);
  ASSERT_NE(out.uri, nullptr);
  EXPECT_STREQ(out.uri, "https://example.com/audio.wav");
  EXPECT_STREQ(out.format, "wav");
}

// ========================================================================
// TensorItem — SetTensorData + GetApiData
// ========================================================================

TEST(TensorItemTest, SetTensorData) {
  TensorItem item;
  float data[] = {1.0f, 2.0f, 3.0f, 4.0f};
  int64_t shape[] = {2, 2};
  flTensorData in = {};
  in.version = FOUNDRY_LOCAL_API_VERSION;
  in.data_type = FOUNDRY_LOCAL_TENSOR_FLOAT;
  in.data = data;
  in.shape = shape;
  in.rank = 2;

  item.SetTensorData(in);

  EXPECT_EQ(item.data_type, FOUNDRY_LOCAL_TENSOR_FLOAT);
  EXPECT_EQ(item.data, static_cast<void*>(data));
  ASSERT_EQ(item.shape.size(), 2u);
  EXPECT_EQ(item.shape[0], 2);
  EXPECT_EQ(item.shape[1], 2);
}

TEST(TensorItemTest, GetApiData_RoundTrip) {
  float data[] = {1.0f};
  TensorItem item(FOUNDRY_LOCAL_TENSOR_FLOAT, data, {1, 1});

  flTensorData out = {};
  item.GetApiData(out);

  EXPECT_EQ(out.version, FOUNDRY_LOCAL_API_VERSION);
  EXPECT_EQ(out.data_type, FOUNDRY_LOCAL_TENSOR_FLOAT);
  EXPECT_EQ(out.data, static_cast<void*>(data));
  ASSERT_EQ(out.rank, 2u);
  EXPECT_EQ(out.shape[0], 1);
  EXPECT_EQ(out.shape[1], 1);
}

// ========================================================================
// BytesItem — SetBytesData + GetApiData
// ========================================================================

TEST(BytesItemTest, SetBytesData) {
  BytesItem item;
  uint8_t raw[] = {0xDE, 0xAD};
  flBytesData in = {};
  in.version = FOUNDRY_LOCAL_API_VERSION;
  in.item_type = FOUNDRY_LOCAL_ITEM_IMAGE;
  in.data = raw;
  in.data_size = sizeof(raw);

  item.SetBytesData(in);

  EXPECT_EQ(item.item_type, FOUNDRY_LOCAL_ITEM_IMAGE);
  EXPECT_EQ(item.data, static_cast<void*>(raw));
  EXPECT_EQ(item.data_size, sizeof(raw));
}

TEST(BytesItemTest, GetApiData_RoundTrip) {
  uint8_t raw[] = {0x01, 0x02, 0x03};
  BytesItem item(FOUNDRY_LOCAL_ITEM_AUDIO, raw, sizeof(raw));

  flBytesData out = {};
  item.GetApiData(out);

  EXPECT_EQ(out.version, FOUNDRY_LOCAL_API_VERSION);
  EXPECT_EQ(out.item_type, FOUNDRY_LOCAL_ITEM_AUDIO);
  EXPECT_EQ(out.data, static_cast<void*>(raw));
  EXPECT_EQ(out.data_size, sizeof(raw));
}

// ========================================================================
// MessageItem — copy constructor
// ========================================================================

TEST(MessageItemTest, CopyConstructor) {
  MessageItem original(FOUNDRY_LOCAL_ROLE_ASSISTANT, "Hello!", "Bot");
  MessageItem copy(original);

  EXPECT_EQ(copy.type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(copy.role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(copy.GetSimpleText(), "Hello!");
  EXPECT_EQ(copy.name, "Bot");

  // Copy constructor deep-clones every part. Clearing `original.content`
  // must not affect the copy.
  original.content.clear();
  EXPECT_EQ(copy.GetSimpleText(), "Hello!");
}

TEST(MessageItemTest, CopyConstructor_PreservesTextItemTextType) {
  std::vector<std::unique_ptr<Item>> parts;
  parts.push_back(std::make_unique<TextItem>("let me think...", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING));
  MessageItem original(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts));

  MessageItem copy(original);

  ASSERT_EQ(copy.content.size(), 1u);
  ASSERT_TRUE(copy.content.front().view);
  ASSERT_EQ(copy.content.front().view->type, FOUNDRY_LOCAL_ITEM_TEXT);
  const auto& cloned_text = static_cast<const TextItem&>(*copy.content.front().view);
  EXPECT_EQ(cloned_text.text, "let me think...");
  EXPECT_EQ(cloned_text.text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
}

// ========================================================================
// AudioItem — sample_rate / channels round-trip
// ========================================================================

TEST(AudioItemTest, SampleRateAndChannels_DefaultsAreZero) {
  AudioItem item;
  EXPECT_EQ(item.sample_rate, 0);
  EXPECT_EQ(item.channels, 0);
}

TEST(AudioItemTest, SampleRateAndChannels_SetViaFlAudioData) {
  AudioItem item;

  flAudioData ad{};
  ad.version = FOUNDRY_LOCAL_API_VERSION;
  ad.format = "pcm";
  ad.sample_rate = 16000;
  ad.channels = 1;
  item.SetAudioData(ad);

  EXPECT_EQ(item.sample_rate, 16000);
  EXPECT_EQ(item.channels, 1);
  EXPECT_EQ(item.format, "pcm");
}

TEST(AudioItemTest, SampleRateAndChannels_GetApiDataRoundTrip) {
  AudioItem item;

  flAudioData ad{};
  ad.version = FOUNDRY_LOCAL_API_VERSION;
  ad.format = "pcm";
  ad.sample_rate = 44100;
  ad.channels = 2;
  item.SetAudioData(ad);

  flAudioData out{};
  item.GetApiData(out);

  EXPECT_EQ(out.sample_rate, 44100);
  EXPECT_EQ(out.channels, 2);
  EXPECT_STREQ(out.format, "pcm");
}

TEST(AudioItemTest, SampleRateAndChannels_ZeroWhenUnset) {
  AudioItem item("path/to/file.mp3", "mp3");

  flAudioData out{};
  item.GetApiData(out);

  EXPECT_EQ(out.sample_rate, 0);
  EXPECT_EQ(out.channels, 0);
  EXPECT_STREQ(out.format, "mp3");
}

// ========================================================================
// ItemQueue — WaitAndPop
// ========================================================================

TEST(ItemQueueTest, WaitAndPop_ReturnsImmediatelyWhenNonEmpty) {
  ItemQueue queue;
  queue.Push(std::make_unique<TextItem>("hello"));

  auto item = queue.WaitAndPop(std::chrono::milliseconds(1000));
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->type, FOUNDRY_LOCAL_ITEM_TEXT);
}

TEST(ItemQueueTest, WaitAndPop_ReturnsNullptrOnTimeout) {
  ItemQueue queue;

  auto start = std::chrono::steady_clock::now();
  auto item = queue.WaitAndPop(std::chrono::milliseconds(50));
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(item, nullptr);
  EXPECT_GE(elapsed, std::chrono::milliseconds(40));  // waited ~50ms
}

TEST(ItemQueueTest, WaitAndPop_WakesOnPush) {
  ItemQueue queue;

  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    queue.Push(std::make_unique<TextItem>("pushed"));
  });

  auto item = queue.WaitAndPop(std::chrono::milliseconds(5000));
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->type, FOUNDRY_LOCAL_ITEM_TEXT);

  producer.join();
}

TEST(ItemQueueTest, WaitAndPop_WakesOnMarkFinished) {
  ItemQueue queue;

  std::thread finisher([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    queue.MarkFinished();
  });

  auto item = queue.WaitAndPop(std::chrono::milliseconds(5000));
  EXPECT_EQ(item, nullptr);
  EXPECT_TRUE(queue.IsFinished());

  finisher.join();
}

TEST(ItemQueueTest, WaitAndPop_DrainsBeforeReturningNullptr) {
  ItemQueue queue;
  queue.Push(std::make_unique<TextItem>("a"));
  queue.Push(std::make_unique<TextItem>("b"));
  queue.MarkFinished();

  auto item1 = queue.WaitAndPop(std::chrono::milliseconds(10));
  ASSERT_NE(item1, nullptr);

  auto item2 = queue.WaitAndPop(std::chrono::milliseconds(10));
  ASSERT_NE(item2, nullptr);

  auto item3 = queue.WaitAndPop(std::chrono::milliseconds(10));
  EXPECT_EQ(item3, nullptr);
}
