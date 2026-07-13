// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Download integration tests — exercises RemoveFromCache + Download code paths.
// Disabled by default because they modify the model cache and require network
// access.  Run with --gtest_also_run_disabled_tests to include them.

#include "model_fixture.h"

#include <iostream>
#include <vector>

// ========================================================================
// DISABLED_DownloadFixture — exercises RemoveFromCache + Download.
// Disabled by default because it modifies the model cache and requires
// network access.  Run with --gtest_also_run_disabled_tests to include
// (e.g. during code-coverage runs).
// ========================================================================

class DISABLED_DownloadFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.model_list()) {
      GTEST_SKIP() << "No catalog available";
    }
  }

  static foundry_local::ModelList& model_list() {
    return *SharedTestEnv::Get().model_list();
  }
};

TEST_F(DISABLED_DownloadFixture, RemoveAndRedownloadSmallestModel) {
  // Find the smallest CPU model to remove+redownload, EXCLUDING the models the shared
  // environment reserves for its modality fixtures. Those (e.g. whisper for the audio
  // fixture, qwen for chat) are loaded by other suites; removing one here would race the
  // fixture that depends on it. Skipping the whole reserved set — not just "not currently
  // loaded" — is what makes this robust: a reserved model may be unloaded right now but
  // loaded by a later suite. Iterate all variants of each alias group — GetModels() only
  // returns the selected variant, which may hide smaller CPU variants behind a GPU pick.
  auto reserved = SharedTestEnv::Get().ReservedModels();

  foundry_local::IModel* target = nullptr;
  int64_t target_size = std::numeric_limits<int64_t>::max();

  for (const auto& m : model_list()) {
    if (m->IsLoaded()) {
      continue;
    }

    if (reserved.count(m.get()) != 0) {
      continue;
    }

    auto variants = m->GetVariants();
    for (const auto& v : variants) {
      auto vi = v->GetInfo();

      if (vi.DeviceType() != FOUNDRY_LOCAL_DEVICE_CPU) {
        continue;
      }

      int64_t size = vi.FilesizeMb().value_or(0);
      if (size > 0 && size < target_size) {
        target_size = size;
        m->SelectVariant(*v);
        target = m.get();
      }
    }
  }

  ASSERT_NE(target, nullptr) << "No unreserved, unloaded CPU model found in catalog";

  auto info = target->GetInfo();
  std::cout << "Download test model: " << info.Name()
            << " (" << info.FilesizeMb().value_or(0) << " MB)\n";

  // Remove from cache to force a real download.
  if (target->IsCached()) {
    target->RemoveFromCache();
  }

  ASSERT_FALSE(target->IsCached())
      << "Model should not be cached after RemoveFromCache";

  // Download with progress tracking.
  std::vector<float> progress_values;
  target->Download([&progress_values](float pct) {
    progress_values.push_back(pct);
    return 0;  // 0 = continue (non-zero would cancel; see flProgressCallback contract).
  });

  EXPECT_TRUE(target->IsCached())
      << "Model should be cached after Download";

  // A real download should produce multiple progress callbacks — not just
  // a single 100% that a cache hit would produce.
  ASSERT_GT(progress_values.size(), 1u)
      << "Expected multiple progress callbacks from a fresh download";

  // Final progress should be 100%.
  EXPECT_FLOAT_EQ(progress_values.back(), 100.0f);

  // Verify progress values are monotonically non-decreasing.
  for (size_t i = 1; i < progress_values.size(); ++i) {
    EXPECT_GE(progress_values[i], progress_values[i - 1])
        << "Progress went backwards at index " << i;
  }

  std::cout << "Download completed with " << progress_values.size()
            << " progress callbacks\n";

  // Load and verify the downloaded model works.
  EXPECT_NO_THROW(target->Load());
  EXPECT_TRUE(target->IsLoaded());

  // Unload — this model isn't used by other fixtures.
  target->Unload();
  EXPECT_FALSE(target->IsLoaded());
}

TEST_F(DISABLED_DownloadFixture, DownloadAlreadyCachedModelIsNoOp) {
  // Grab the chat model that SharedTestEnv already downloaded.
  auto* model = SharedTestEnv::Get().chat_model();
  if (!model) {
    GTEST_SKIP() << "No chat model available";
  }

  ASSERT_TRUE(model->IsCached()) << "Chat model should already be cached";

  // Download again — should be a no-op but still call progress with 100%.
  std::vector<float> progress_values;
  model->Download([&progress_values](float pct) {
    progress_values.push_back(pct);
    return 0;  // 0 = continue (non-zero would cancel; see flProgressCallback contract).
  });

  EXPECT_TRUE(model->IsCached());

  // For an already-cached model the callback should still fire with 100%.
  ASSERT_FALSE(progress_values.empty());
  EXPECT_FLOAT_EQ(progress_values.back(), 100.0f);
}

TEST_F(DISABLED_DownloadFixture, ResumesPartialDownloadAfterCancel) {
  // Live resume check: cancel a real download partway, then re-run and confirm the
  // .dlstate sidecar drove a *partial* resume rather than a fresh re-download.
  // Pick the smallest uncached CPU model (same selection as the redownload test).
  foundry_local::IModel* target = nullptr;
  int64_t target_size = std::numeric_limits<int64_t>::max();
  for (const auto& m : model_list()) {
    if (m->IsLoaded()) {
      continue;
    }
    for (const auto& v : m->GetVariants()) {
      auto vi = v->GetInfo();
      if (vi.DeviceType() != FOUNDRY_LOCAL_DEVICE_CPU) {
        continue;
      }
      int64_t size = vi.FilesizeMb().value_or(0);
      if (size > 0 && size < target_size) {
        target_size = size;
        m->SelectVariant(*v);
        target = m.get();
      }
    }
  }
  ASSERT_NE(target, nullptr) << "No unloaded CPU model found in catalog";

  if (target->IsCached()) {
    target->RemoveFromCache();
  }
  ASSERT_FALSE(target->IsCached());

  // First attempt: cancel once aggregate progress passes ~30%, leaving partial
  // data plus its .dlstate sidecar(s) on disk. The progress callback returns 0 to
  // continue and non-zero to cancel.
  float cancel_pct = -1.0f;
  bool threw = false;
  try {
    target->Download([&cancel_pct](float pct) -> int {
      if (pct >= 30.0f) {
        cancel_pct = pct;
        return 1;  // cancel
      }
      return 0;  // continue
    });
  } catch (const std::exception&) {
    threw = true;  // cancellation surfaces as a thrown error
  }
  ASSERT_GE(cancel_pct, 30.0f) << "download never reached the cancel threshold";
  EXPECT_TRUE(threw) << "a cancelled download should surface an error";
  EXPECT_FALSE(target->IsCached()) << "a cancelled download must not report cached";

  // Second attempt: let it finish, capturing every reported percentage.
  std::vector<float> resume_progress;
  target->Download([&resume_progress](float pct) -> int {
    resume_progress.push_back(pct);
    return 0;
  });

  ASSERT_FALSE(resume_progress.empty());

  // DownloadManager::DownloadModel always emits a 0% heartbeat (progress_cb(0.0f))
  // before the transfer starts, which Model::Download forwards unchanged, so
  // resume_progress.front() is that heartbeat -- not the resumed percentage. Skip
  // the leading zero(s); the first non-zero sample is the initial on-disk
  // reflection DownloadBlobsToDirectory emits from the bytes already present.
  float first_real = 0.0f;
  for (float pct : resume_progress) {
    if (pct > 0.0f) {
      first_real = pct;
      break;
    }
  }
  ASSERT_GT(first_real, 0.0f) << "resume produced no real progress past the 0% heartbeat";

  // A sidecar-driven partial resume reports its first real progress at roughly the
  // bytes already on disk (~the cancel point), so it lands well above the tiny
  // first-chunk fraction a fresh re-download would start from. The threshold is
  // intentionally lenient (sidecar saves are bounded at ~16 MB granularity); tune
  // it if a different model than the smallest CPU one is selected.
  constexpr float kMinResumeProgressPct = 10.0f;
  EXPECT_GE(first_real, kMinResumeProgressPct)
      << "first real progress " << first_real << "% looks like a fresh re-download, not a resume";
  EXPECT_FLOAT_EQ(resume_progress.back(), 100.0f);
  EXPECT_TRUE(target->IsCached());
}
