// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_coordinator.h"

#include "exception.h"
#include "hosting/host_process.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class HandshakeDeadline {
 public:
  HandshakeDeadline(SdkConnection& connection, Clock::time_point deadline)
      : worker_([this, &connection, deadline] {
          std::unique_lock lock(mutex_);
          if (!changed_.wait_until(lock, deadline, [this] { return complete_; })) {
            connection.Close();
          }
        }) {}

  ~HandshakeDeadline() {
    {
      std::lock_guard lock(mutex_);
      complete_ = true;
    }

    changed_.notify_all();
    worker_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool complete_ = false;
  std::thread worker_;
};

std::unique_ptr<ProcessWatch> ObserveProcess(ProcessIdentity identity, Clock::time_point deadline) {
  for (;;) {
    if (ProbeProcessIdentity(identity) == ProcessIdentityProbe::kAbsent) {
      return nullptr;
    }
    try {
      return std::make_unique<ProcessWatch>(identity);
    } catch (const ProcessWatchError& error) {
      if (error.Failure() != ProcessWatchFailure::kStaleIdentity) {
        throw;
      }
    }
    if (Clock::now() >= deadline) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host process observation exceeded its startup deadline");
    }

    std::this_thread::sleep_for(1ms);
  }
}

}  // namespace

HostCoordinator::HostCoordinator(const Configuration& configuration, RuntimeAssets assets, std::filesystem::path root,
                                 std::chrono::milliseconds startup_timeout)
    : assets_(std::move(assets)),
      profile_(BuildHostProfile(configuration, assets_.identity)),
      store_(profile_.key, std::move(root)),
      startup_timeout_(startup_timeout) {
  if (startup_timeout_ <= 0ms) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host startup timeout must be positive");
  }
}

std::shared_ptr<api::HostedContext> HostCoordinator::Attach(const HostLaunchRecord& record, bool acquire,
                                                            ProcessWatch::TimePoint deadline) const {
  auto channel = TryConnect(record.child.endpoint, deadline);
  if (!channel) {
    return nullptr;
  }

  const auto peer = channel->Peer();
  if (peer.process_id != record.process->pid || (peer.generation && *peer.generation != record.process->generation)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host endpoint belongs to a different process");
  }

  auto connection = std::make_shared<SdkConnection>(std::move(*channel), SdkConnection::Side::Client);
  const auto owner = CurrentProcessIdentity();
  HandshakeDeadline timeout(*connection, std::min(deadline, Clock::now() + 5s));
  const auto reply =
      connection->Call(Operation::Handshake, {{"protocol", SdkConnection::kProtocolVersion},
                                              {"profile", profile_.key},
                                              {"owner", acquire},
                                              {"process", {{"pid", owner.pid}, {"generation", owner.generation}}}});
  if (reply.value.is_null()) {
    if (reply.status == 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host returned an empty handshake acknowledgment");
    }

    FL_THROW(static_cast<flErrorCode>(reply.status), "native host handshake failed: ", reply.error);
  }

  const auto& value = reply.value;
  if (value.at("protocol") != SdkConnection::kProtocolVersion || value.at("profile") != profile_.key ||
      value.at("incarnation") != record.child.incarnation || value.at("process").at("pid") != record.process->pid ||
      value.at("process").at("generation") != record.process->generation ||
      !value.at("connection").is_number_unsigned() || value.at("connection").get<uint64_t>() == 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host handshake does not match the selected incarnation");
  }
  if (acquire && !value.at("owner").get<bool>()) {
    if (reply.status != FOUNDRY_LOCAL_ERROR_INVALID_USAGE) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host did not acknowledge owner attachment");
    }

    return nullptr;
  }
  if (reply.status != 0 || (!acquire && value.at("owner").get<bool>())) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host rejected the expected connection scope: ", reply.error);
  }

  return std::make_shared<api::HostedContext>(connection, record.child.incarnation,
                                              value.at("connection").get<uint64_t>());
}

std::shared_ptr<api::HostedContext> HostCoordinator::Connect(bool acquire) const {
  if (!acquire && !store_.Exists()) {
    return nullptr;
  }

  const auto deadline = Clock::now() + startup_timeout_;
  const auto lock = store_.Lock(deadline);
  auto record = store_.Read();
  std::unique_ptr<ProcessWatch> process;
  bool launched = false;
  for (;;) {
    if (!record) {
      if (!acquire) {
        return nullptr;
      }

      const auto incarnation = NewHostIncarnation();
      record = HostLaunchRecord{{store_.Endpoint(incarnation), profile_.key, incarnation, CurrentProcessIdentity()},
                                std::nullopt};
#ifndef _WIN32
      record->startup_lease = true;
#endif
      auto descriptor = assets_.bootstrap;
      descriptor.child = record->child;
      descriptor.compatibility = profile_.compatibility;
      descriptor.configuration = profile_.configuration;
      VerifyBootstrapArtifacts(descriptor, assets_.executable, descriptor.native_build, descriptor.runtime_identifier);
      WriteHostJson(store_.BootstrapPath(*record), EncodeBootstrapDescriptor(descriptor));
      store_.Write(*record);
      try {
        record->process = SpawnNativeHost(assets_.executable, store_.BootstrapPath(*record), *lock, deadline);
      } catch (const HostLaunchRejected&) {
        store_.ForgetRejected(*record);
        throw;
      }

      launched = true;
      store_.Write(*record);
    }
    if (!record->process) {
      record->process = store_.ReadPublishedProcess(*record);
      if (record->process) {
        store_.Write(*record);
      } else {
        if (record->startup_lease && !launched) {
          store_.ForgetAbandoned(*record);
          record.reset();
          continue;
        }

        auto starter = ObserveProcess(record->child.starter, deadline);
        if (!starter || starter->IsExited()) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                   "interrupted native host launch has no confirmed child identity; preserving reconciliation state");
        }
      }
    }
    if (record->process) {
      if (!process) {
        process = ObserveProcess(*record->process, deadline);
      }
      if (!process || process->IsExited()) {
        const auto failure = launched ? store_.ReadStartupFailure(*record) : std::string{};
        store_.ForgetExited(*record);
        record.reset();
        process.reset();
        if (launched) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "native host exited before owner attachment",
                   failure.empty() ? "" : ": ", failure);
        }

        continue;
      }

      if (auto context = Attach(*record, acquire, deadline)) {
        return context;
      }
    }
    if (Clock::now() >= deadline) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "native host startup/exit could not be confirmed before its deadline; preserving reconciliation state");
    }

    std::this_thread::sleep_for(10ms);
  }
}

api::HostedConnector CreateNativeHostConnector(const Configuration& configuration) {
  auto coordinator = std::make_shared<HostCoordinator>(configuration, ResolveRuntimeAssets());
  return [coordinator = std::move(coordinator)](bool acquire) { return coordinator->Connect(acquire); };
}

}  // namespace fl::hosting
