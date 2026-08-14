// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "manager.h"

#include "configuration.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/model_load_manager.h"
#include "internal_api/test_model_cache.h"

#include <gtest/gtest.h>
#include <ort_genai.h>

#include <string>

namespace fl {
namespace {

constexpr const char* kTestModelAlias = "tiny-random-gpt2-fp32-1";

class ManagerDestroyGuard {
 public:
  ~ManagerDestroyGuard() { Manager::Destroy(); }
};

TEST(ManagerReinitTest, LoadsModelAfterShutdown) {
  const auto model_path = test::GetTestDataPath(kTestModelAlias);

  for (int cycle = 0; cycle < 2; ++cycle) {
    SCOPED_TRACE("Manager lifecycle cycle " + std::to_string(cycle));
    ManagerDestroyGuard manager_guard;

    Configuration config;
    config.app_name = "manager-reinit-test";
    config.disable_nonessential_telemetry = true;

    auto& manager = Manager::Create(config);
    auto result = manager.GetModelLoadManager().LoadModel(model_path.string(), kTestModelAlias);
    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess);
    ASSERT_NE(result.model, nullptr);

    auto sequences = result.model->Tokenizer().Encode("Hello");
    EXPECT_GT(sequences->SequenceCount(0), 0u);
  }
}

}  // namespace
}  // namespace fl
