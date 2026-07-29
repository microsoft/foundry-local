// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// C++ wrapper API tests — Item types, Request/Response, KeyValuePairs, etc.
// These test the public C++ API without requiring a model or network.
#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

TEST(CppApiTest, TextItemDefaultRoundTrip) {
  auto item = foundry_local::Item::Text("hello world");
  EXPECT_EQ(item.GetType(), FOUNDRY_LOCAL_ITEM_TEXT);

  auto content = item.GetText();
  EXPECT_EQ(content.text, "hello world");
  EXPECT_EQ(content.type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
}

TEST(CppApiTest, TextItemReasoningRoundTrip) {
  auto item = foundry_local::Item::Text("let me think...", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
  EXPECT_EQ(item.GetType(), FOUNDRY_LOCAL_ITEM_TEXT);

  auto content = item.GetText();
  EXPECT_EQ(content.text, "let me think...");
  EXPECT_EQ(content.type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING);
}

TEST(CppApiTest, MessageItemRoundTrip) {
  auto item = foundry_local::MessageItem(FOUNDRY_LOCAL_ROLE_USER, "Hi there", "Alice");
  EXPECT_EQ(item.GetType(), FOUNDRY_LOCAL_ITEM_MESSAGE);

  auto msg = item.GetMessage();
  EXPECT_EQ(msg.role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(msg.GetSimpleText(), "Hi there");
  ASSERT_TRUE(msg.name.has_value());
  EXPECT_EQ(*msg.name, "Alice");
}

TEST(CppApiTest, ConvenienceMessageClasses) {
  auto sys = foundry_local::SystemMessage("Be helpful.");
  EXPECT_EQ(sys.GetMessage().role, FOUNDRY_LOCAL_ROLE_SYSTEM);
  EXPECT_EQ(sys.GetMessage().GetSimpleText(), "Be helpful.");

  auto user = foundry_local::UserMessage("What is 2+2?");
  EXPECT_EQ(user.GetMessage().role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(user.GetMessage().GetSimpleText(), "What is 2+2?");

  auto asst = foundry_local::AssistantMessage("4");
  EXPECT_EQ(asst.GetMessage().role, FOUNDRY_LOCAL_ROLE_ASSISTANT);
  EXPECT_EQ(asst.GetMessage().GetSimpleText(), "4");

  auto dev = foundry_local::DeveloperMessage("debug info");
  EXPECT_EQ(dev.GetMessage().role, FOUNDRY_LOCAL_ROLE_DEVELOPER);
  EXPECT_EQ(dev.GetMessage().GetSimpleText(), "debug info");
}

TEST(CppApiTest, TensorItemRoundTrip) {
  float data[] = {1.0f, 2.0f, 3.0f, 4.0f};
  int64_t shape[] = {2, 2};
  auto item = foundry_local::Item::Tensor(FOUNDRY_LOCAL_TENSOR_FLOAT, data, shape, 2);
  EXPECT_EQ(item.GetType(), FOUNDRY_LOCAL_ITEM_TENSOR);

  auto [data_type, tensor_data, tensor_shape] = item.GetTensor();
  EXPECT_EQ(data_type, FOUNDRY_LOCAL_TENSOR_FLOAT);
  ASSERT_EQ(tensor_shape.size(), 2u);
  EXPECT_EQ(tensor_shape[0], 2);
  EXPECT_EQ(tensor_shape[1], 2);
}

TEST(CppApiTest, ImageItemBytesRoundTrip) {
  uint8_t pixels[] = {0xFF, 0x00, 0xFF, 0x00};
  auto item = foundry_local::Item::ImageFromData("png", pixels, sizeof(pixels));
  EXPECT_EQ(item.GetType(), FOUNDRY_LOCAL_ITEM_IMAGE);

  auto [data, data_size, format, uri] = item.GetImage();
  EXPECT_EQ(data_size, sizeof(pixels));
  ASSERT_TRUE(format.has_value());
  EXPECT_EQ(*format, "png");
  EXPECT_FALSE(uri.has_value());
}

TEST(CppApiTest, ImageItemUriRoundTrip) {
  auto item = foundry_local::Item::ImageFromUri("https://example.com/test.jpg", std::string("jpeg"));
  EXPECT_EQ(item.GetType(), FOUNDRY_LOCAL_ITEM_IMAGE);

  auto [data, data_size, format, uri] = item.GetImage();
  ASSERT_TRUE(uri.has_value());
  EXPECT_EQ(*uri, "https://example.com/test.jpg");
  ASSERT_TRUE(format.has_value());
  EXPECT_EQ(*format, "jpeg");
  EXPECT_EQ(data, nullptr);
  EXPECT_EQ(data_size, 0u);
}

TEST(CppApiTest, BytesItemRoundTrip) {
  uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  auto item = foundry_local::Item::Bytes(FOUNDRY_LOCAL_ITEM_AUDIO, data, sizeof(data));
  EXPECT_EQ(item.GetType(), FOUNDRY_LOCAL_ITEM_BYTES);

  auto [item_type, out_data, data_size] = item.GetBytes();
  EXPECT_EQ(item_type, FOUNDRY_LOCAL_ITEM_AUDIO);
  EXPECT_EQ(data_size, sizeof(data));
  EXPECT_EQ(std::memcmp(out_data, data, sizeof(data)), 0);
}

TEST(CppApiTest, ToolCallRoundTrip) {
  auto tc = foundry_local::Item::ToolCall("call_42", "get_weather", R"({"city":"Seattle"})");
  EXPECT_EQ(tc.GetType(), FOUNDRY_LOCAL_ITEM_TOOL_CALL);

  auto [call_id, name, arguments] = tc.GetToolCall();
  EXPECT_EQ(call_id, "call_42");
  EXPECT_EQ(name, "get_weather");
  EXPECT_EQ(arguments, R"({"city":"Seattle"})");
}

TEST(CppApiTest, ToolResultRoundTrip) {
  auto tr = foundry_local::Item::ToolResult("call_42", "72 degrees");
  EXPECT_EQ(tr.GetType(), FOUNDRY_LOCAL_ITEM_TOOL_RESULT);

  auto [call_id, result] = tr.GetToolResult();
  EXPECT_EQ(call_id, "call_42");
  EXPECT_EQ(result, "72 degrees");
}

TEST(CppApiTest, ItemQueuePushPopFinish) {
  foundry_local::ItemQueue queue;
  EXPECT_EQ(queue.GetType(), FOUNDRY_LOCAL_ITEM_QUEUE);
  EXPECT_EQ(queue.Size(), 0u);
  EXPECT_FALSE(queue.IsFinished());

  // Push items
  queue.Push(foundry_local::Item::Text("first"));
  queue.Push(foundry_local::Item::Text("second"));
  EXPECT_EQ(queue.Size(), 2u);

  // Pop first
  auto item1 = queue.TryPop();
  ASSERT_TRUE(item1.has_value());
  EXPECT_EQ(item1->GetType(), FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(item1->GetText().text, "first");

  EXPECT_EQ(queue.Size(), 1u);

  // Mark finished
  queue.MarkFinished();
  EXPECT_TRUE(queue.IsFinished());

  // Pop second
  auto item2 = queue.TryPop();
  ASSERT_TRUE(item2.has_value());
  EXPECT_EQ(item2->GetText().text, "second");

  // Empty
  auto empty = queue.TryPop();
  EXPECT_FALSE(empty.has_value());
}

TEST(CppApiTest, RequestVariadicConstructor) {
  foundry_local::Request request{
      foundry_local::SystemMessage("You are helpful."),
      foundry_local::UserMessage("Hello"),
  };

  EXPECT_EQ(request.GetItemCount(), 2u);

  auto item0 = request.GetItem(0);
  EXPECT_EQ(item0.GetType(), FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(item0.GetMessage().role, FOUNDRY_LOCAL_ROLE_SYSTEM);
  EXPECT_EQ(item0.GetMessage().GetSimpleText(), "You are helpful.");

  auto item1 = request.GetItem(1);
  EXPECT_EQ(item1.GetType(), FOUNDRY_LOCAL_ITEM_MESSAGE);
  EXPECT_EQ(item1.GetMessage().role, FOUNDRY_LOCAL_ROLE_USER);
  EXPECT_EQ(item1.GetMessage().GetSimpleText(), "Hello");
}

TEST(CppApiTest, RequestBorrowsItem) {
  auto msg = foundry_local::UserMessage("borrowed");

  {
    foundry_local::Request request;
    request.AddItem(msg, /*take_ownership*/ false);
    EXPECT_EQ(request.GetItemCount(), 1u);

    auto item = request.GetItem(0);
    EXPECT_EQ(item.GetMessage().GetSimpleText(), "borrowed");
  }

  // msg should still be valid
  EXPECT_EQ(msg.GetMessage().GetSimpleText(), "borrowed");
}

TEST(CppApiTest, RequestOwnsMovedItem) {
  foundry_local::Request request;

  auto user_msg = foundry_local::UserMessage("owned");
  request.AddItem(user_msg, /*take_ownership*/ true);  // moves flItem pointer into underlying flRequest
  EXPECT_EQ(request.GetItemCount(), 1u);
  EXPECT_EQ(user_msg.native_handle(), nullptr);  // Moved-from, should be empty

  auto item = request.GetItem(0);
  EXPECT_EQ(item.GetMessage().GetSimpleText(), "owned");
}

TEST(CppApiTest, RequestSetOptions) {
  foundry_local::Request request;
  // Should not throw
  foundry_local::RequestOptions opts;
  opts.search.temperature = 0.5f;
  opts.search.max_output_tokens = 100;
  request.SetOptions(opts);
}

TEST(CppApiTest, RequestCancelOnIdleRequest) {
  foundry_local::Request request;
  // Cancel on an idle request should succeed (no-op)
  EXPECT_NO_THROW(request.Cancel());
}

namespace {

// RAII counter shared via shared_ptr; we capture it inside the std::function
// deleter so that whenever the deleter is destroyed (either via the C-API
// invoking it, or via the wrapper destroying the helper on the failure path)
// the counter drops back to zero. If the wrapper leaks the DataDeleterHelper,
// the captured shared_ptr is never released and live stays > 0.
struct LiveCounter {
  std::atomic<int> live{0};
};

struct CaptureProbe {
  std::shared_ptr<LiveCounter> counter;
  explicit CaptureProbe(std::shared_ptr<LiveCounter> c) : counter(std::move(c)) {
    counter->live.fetch_add(1);
  }
  CaptureProbe(const CaptureProbe& other) : counter(other.counter) {
    if (counter) {
      counter->live.fetch_add(1);
    }
  }
  CaptureProbe(CaptureProbe&& other) noexcept : counter(std::move(other.counter)) {}
  ~CaptureProbe() {
    if (counter) {
      counter->live.fetch_sub(1);
    }
  }
  CaptureProbe& operator=(const CaptureProbe&) = delete;
  CaptureProbe& operator=(CaptureProbe&&) = delete;
};

}  // namespace

// Forcing Item_SetBytes to fail (null data + non-zero size) must not leak the
// DataDeleterHelper that the wrapper allocates for the user-provided deleter.
TEST(CppApiTest, BytesItemDeleterDoesNotLeakOnSetFailure) {
  auto counter = std::make_shared<LiveCounter>();

  EXPECT_THROW({
    CaptureProbe probe(counter);
    foundry_local::Item::Bytes(
        FOUNDRY_LOCAL_ITEM_AUDIO,
        /*data=*/nullptr,
        /*data_size=*/16,
        [probe = std::move(probe)](const flBytesData*) {}); }, foundry_local::Error);

  // The wrapper must destroy the helper (and hence the captured probe) on the
  // failure path; otherwise live stays at 1.
  EXPECT_EQ(counter->live.load(), 0);
}

// Same scenario for the success path: wrapper releases the helper to the C API,
// which calls the deleter (and destroys it) exactly once when the Item is
// destroyed.
TEST(CppApiTest, BytesItemDeleterCalledOnceOnSuccess) {
  auto counter = std::make_shared<LiveCounter>();
  std::vector<uint8_t> raw = {0x01, 0x02, 0x03, 0x04};

  {
    CaptureProbe probe(counter);
    auto item = foundry_local::Item::Bytes(
        FOUNDRY_LOCAL_ITEM_AUDIO,
        raw.data(),
        raw.size(),
        [probe = std::move(probe)](const flBytesData*) {});
    EXPECT_GE(counter->live.load(), 1);
  }

  EXPECT_EQ(counter->live.load(), 0);
}

TEST(CppApiTest, KeyValuePairsBasicOps) {
  foundry_local::KeyValuePairs kvps;

  kvps.Set("key1", "value1");
  kvps.Set("key2", "value2");

  auto v1 = kvps.Get("key1");
  ASSERT_TRUE(v1.has_value());
  EXPECT_EQ(*v1, "value1");

  auto v2 = kvps.Get("key2");
  ASSERT_TRUE(v2.has_value());
  EXPECT_EQ(*v2, "value2");

  EXPECT_FALSE(kvps.Get("nonexistent").has_value());

  kvps.Remove("key1");
  EXPECT_FALSE(kvps.Get("key1").has_value());
}

TEST(CppApiTest, KeyValuePairsInitializerList) {
  foundry_local::KeyValuePairs kvps{
      {"a", "1"},
      {"b", "2"},
      {"c", "3"},
  };

  auto all = kvps.GetAll();
  EXPECT_EQ(all.size(), 3u);
  EXPECT_EQ(*kvps.Get("a"), "1");
  EXPECT_EQ(*kvps.Get("b"), "2");
  EXPECT_EQ(*kvps.Get("c"), "3");
}

TEST(CppApiTest, ConfigurationChaining) {
  // Configuration setters return *this for chaining
  foundry_local::Configuration config("test_chaining");
  config.SetDefaultLogLevel(FOUNDRY_LOCAL_LOG_DEBUG)
      .AddWebServiceEndpoint("http://127.0.0.1:0");
  // Should not throw — just verify chaining compiles and runs
}

TEST(CppApiTest, ConfigurationAddCatalogChaining) {
  // AddCatalog participates in the fluent chaining surface.
  foundry_local::Configuration config("test_add_catalog");
  config.AddCatalog("first", "https://example.com/first")
      .AddCatalog("second", "https://example.com/second");
  // Should not throw — just verify chaining compiles and runs.
}

TEST(CppApiTest, ManagerListCatalogNamesDefaultsToPublic) {
  foundry_local::Manager manager(foundry_local::Configuration("test_default_catalog"));

  auto names = manager.ListCatalogNames();
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0], "public");
}

TEST(CppApiTest, ManagerNamedCatalogsResolveByName) {
  foundry_local::Configuration config("test_named_catalogs");
  config.AddCatalog("first", "https://example.com/first")
      .AddCatalog("second", "https://example.com/second");
  foundry_local::Manager manager(std::move(config));

  auto names = manager.ListCatalogNames();
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[0], "first");
  EXPECT_EQ(names[1], "second");

  // Named lookup resolves each catalog; the no-arg GetCatalog() returns the first (default).
  auto& first = manager.GetCatalog("first");
  auto& second = manager.GetCatalog("second");
  EXPECT_NE(&first, &second);
  EXPECT_EQ(&manager.GetCatalog(), &first);

  // Repeated lookups return the same cached wrapper.
  EXPECT_EQ(&manager.GetCatalog("first"), &first);
}

TEST(CppApiTest, ManagerGetCatalogUnknownNameThrows) {
  foundry_local::Manager manager(foundry_local::Configuration("test_unknown_catalog"));

  EXPECT_THROW(manager.GetCatalog("does-not-exist"), foundry_local::Error);
}

TEST(CppApiTest, ErrorFromCode) {
  foundry_local::Error err("test error", FOUNDRY_LOCAL_ERROR_INTERNAL);
  EXPECT_EQ(err.Code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
  EXPECT_NE(std::string(err.what()).find("test error"), std::string::npos);
}

TEST(CppApiTest, VersionStringNonEmpty) {
  const char* version = foundry_local::Version();
  ASSERT_NE(version, nullptr);
  EXPECT_GT(std::strlen(version), 0u);
}
