// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "hosting/child_host_role.h"

#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace fl::hosting {

struct HostLaunchRecord {
  ChildHostRole child;
  std::optional<ProcessIdentity> process;
  bool startup_lease = false;
};

// The startup lock is neither a host-lifetime reference nor evidence that a predecessor exited.
class HostStartupLock {
 public:
  HostStartupLock(const std::filesystem::path& directory, std::chrono::steady_clock::time_point deadline);
  ~HostStartupLock();
  HostStartupLock(const HostStartupLock&) = delete;
  HostStartupLock& operator=(const HostStartupLock&) = delete;
#ifndef _WIN32
  int Descriptor() const noexcept;
#endif

 private:
  struct State;
  std::unique_ptr<State> state_;
};

class HostStateStore {
 public:
  // The explicit root is an internal test seam, not a public configuration or environment override.
  explicit HostStateStore(std::string profile_key, std::filesystem::path root = {});
  bool Exists() const;
  std::unique_ptr<HostStartupLock> Lock(std::chrono::steady_clock::time_point deadline) const;
  std::optional<HostLaunchRecord> Read() const;
  void Write(const HostLaunchRecord& record) const;
  std::optional<ProcessIdentity> ReadPublishedProcess(const HostLaunchRecord& record) const;
  // Requires the startup lock and OS-confirmed predecessor exit. Never use missing IPC/files as exit evidence.
  void ForgetExited(const HostLaunchRecord& record) const;
  // Only for a launch that the OS rejected before creating a child.
  void ForgetRejected(const HostLaunchRecord& record) const;
  // The acquired startup lock proves an inherited launch lease no longer has an unpublished child.
  void ForgetAbandoned(const HostLaunchRecord& record) const;
  std::filesystem::path BootstrapPath(const HostLaunchRecord& record) const;
  std::filesystem::path Endpoint(const std::string& incarnation) const;
  const std::filesystem::path& Directory() const noexcept { return directory_; }

 private:
  void Validate(const HostLaunchRecord& record) const;
  std::string profile_key_;
  std::filesystem::path root_;
  std::filesystem::path directory_;
};

std::string NewHostIncarnation();
void WriteHostJson(const std::filesystem::path& path, const nlohmann::json& value);
// Publish before runtime loading, allowing another starter to reconcile an interrupted parent without guessing a PID.
void PublishHostProcess(const std::filesystem::path& bootstrap_path, const ChildHostRole& child,
                        ProcessIdentity process);
void PublishHostReady(const std::filesystem::path& bootstrap_path, const ChildHostRole& child, ProcessIdentity process);

}  // namespace fl::hosting
