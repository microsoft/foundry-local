// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_state.h"

#include "exception.h"
#include "hosting/bootstrap_descriptor.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <chrono>

#ifdef _WIN32
#include "hosting/windows_user.h"
#else
#include <sys/stat.h>
#endif

namespace fl::hosting {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class HostStateTest : public testing::Test {
 protected:
  void SetUp() override {
    const auto scratch = std::filesystem::path(FOUNDRY_LOCAL_TEST_SCRATCH_DIR);
    ASSERT_TRUE(scratch.is_absolute());
    std::filesystem::create_directories(scratch);
    root_ = scratch / ("host-state-" + std::to_string(test::CurrentPid()) + "-" + NewHostIncarnation());
    ASSERT_FALSE(std::filesystem::exists(root_));
    root_ = HostStateStore(profile_, root_).Directory().parent_path();
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  HostLaunchRecord Record(const HostStateStore& store) const {
    return {{store.Endpoint(), profile_, NewHostIncarnation(), CurrentProcessIdentity()}, std::nullopt};
  }

  const std::string profile_ = std::string(64, 'A');
  std::filesystem::path root_;
};

TEST_F(HostStateTest, AbsentObservationDoesNotCreateDirectoriesOrLaunchRecords) {
  const HostStateStore store(profile_, root_);
  EXPECT_FALSE(store.Exists());
  EXPECT_FALSE(store.Read());
  EXPECT_FALSE(std::filesystem::exists(root_));
  EXPECT_THROW(HostStateStore("not-a-profile", root_), Exception);
  EXPECT_THROW(HostStateStore(profile_, "relative"), Exception);
}

TEST_F(HostStateTest, SerializesIndependentAcquirersWithABoundedDeadlineAndKeepsTheLockInode) {
  const HostStateStore store(profile_, root_);
  auto first = store.Lock(Clock::now() + 1s);
  ASSERT_TRUE(store.Exists());
  EXPECT_THROW(store.Lock(Clock::now() + 20ms), Exception);
  first.reset();
  EXPECT_TRUE(std::filesystem::is_regular_file(store.Directory() / "startup.lock"));
  EXPECT_NO_THROW(store.Lock(Clock::now() + 1s));
}

TEST_F(HostStateTest, PendingLaunchAndChildProcessPublicationRemainDistinctUntilReconciled) {
  const HostStateStore store(profile_, root_);
  const auto lock = store.Lock(Clock::now() + 1s);
  auto record = Record(store);
  store.Write(record);
  auto pending = store.Read();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->process);
  EXPECT_FALSE(store.ReadPublishedProcess(*pending));
  const auto process = CurrentProcessIdentity();
  PublishHostProcess(store.BootstrapPath(record), record.child, process);
  EXPECT_EQ(store.ReadPublishedProcess(record), process);
  EXPECT_FALSE(store.Read()->process);
  record.process = process;
  store.Write(record);
  EXPECT_EQ(store.Read()->process, process);

  auto other = record;
  other.child.incarnation = NewHostIncarnation();
  EXPECT_FALSE(store.ReadPublishedProcess(other));
}

TEST_F(HostStateTest, RejectsForgedProfileTraversalProcessAndPublicationIdentity) {
  const HostStateStore store(profile_, root_);
  const auto lock = store.Lock(Clock::now() + 1s);
  auto record = Record(store);
  auto invalid = record;
  invalid.child.incarnation = "../outside";
  EXPECT_THROW(store.Write(invalid), Exception);
  invalid = record;
  invalid.child.profile_key = std::string(64, 'B');
  EXPECT_THROW(store.Write(invalid), Exception);
  store.Write(record);
  auto encoded = ReadBootstrapJson(store.Directory() / "launch.json");
  encoded["process"] = {{"pid", 1.5}, {"generation", 1}};
  WriteHostJson(store.Directory() / "launch.json", encoded);
  EXPECT_THROW(store.Read(), Exception);
  auto wrong_child = record.child;
  wrong_child.profile_key = std::string(64, 'B');
  PublishHostProcess(store.BootstrapPath(record), wrong_child, CurrentProcessIdentity());
  EXPECT_THROW(store.ReadPublishedProcess(record), Exception);
}

TEST_F(HostStateTest, RejectsExistingDirectoriesThatGrantOtherUsersAccess) {
#ifdef _WIN32
  const auto sddl = L"O:" + windows::UserSidText() + L"D:P(A;;FA;;;WD)";
  PSECURITY_DESCRIPTOR raw = nullptr;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr));
  const windows::LocalMemory descriptor(raw);
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
  ASSERT_TRUE(CreateDirectoryW(root_.c_str(), &security));
#else
  ASSERT_EQ(mkdir(root_.c_str(), 0700), 0);
  ASSERT_EQ(chmod(root_.c_str(), 0755), 0);
#endif
  const HostStateStore store(profile_, root_);
  EXPECT_THROW(store.Exists(), Exception);
  EXPECT_THROW(store.Lock(Clock::now() + 20ms), Exception);
}

}  // namespace
}  // namespace fl::hosting
