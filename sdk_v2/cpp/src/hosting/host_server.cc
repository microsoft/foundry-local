// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_server.h"

#include "exception.h"
#include "hosting/host_objects.h"
#include "hosting/manager_dispatcher.h"
#include "hosting/model_dispatcher.h"
#include "hosting/session_dispatcher.h"
#include "util/scope_guard.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

HostLifetime& RequireLifetime(const std::shared_ptr<HostLifetime>& lifetime) {
  if (!lifetime) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host requires a lifetime controller");
  }

  return *lifetime;
}

bool IsObservation(Operation operation) noexcept {
  switch (operation) {
    case Operation::Observe:
    case Operation::WebServiceUrls:
    case Operation::DiscoverableEps:
    case Operation::RuntimeDevices:
    case Operation::LoadedModels:
    case Operation::IsLoaded:
    case Operation::ModelInputOutputInfo:
      return true;
    default:
      return false;
  }
}

}  // namespace

struct HostServer::Impl {
  struct Peer {
    struct Worker {
      std::shared_ptr<std::atomic<bool>> done;
      std::thread thread;
    };

    Peer(Impl& host_in, Channel channel, PeerIdentity identity_in, uint64_t generation_in)
        : host(host_in),
          identity(identity_in),
          generation(generation_in),
          handshake_deadline(host.Now() + 5s),
          connection(std::make_unique<SdkConnection>(std::move(channel), SdkConnection::Side::Server)) {}

    ~Peer() {
      Close();
      if (thread.joinable()) {
        thread.join();
      }
    }

    void Close() noexcept {
      std::shared_ptr<HostObjects> retired;
      {
        std::lock_guard lock(mutex);
        if (closed) {
          return;
        }

        closed = true;
        retired = objects;
        if (owner) {
          static_cast<void>(host.lifetime->RemoveOwner(generation, host.Now()));
          owner = false;
        }
      }

      // Retire lookup/cancel first; closing transport then wakes callbacks, input producers and blocked senders.
      if (retired) {
        retired->Close();
      }

      connection->Close();
    }

    bool MustClose(HostLifetime::TimePoint now) {
      std::lock_guard lock(mutex);
      return !closed && (watch ? watch->IsExited() : now >= handshake_deadline);
    }

    void Handshake(const std::shared_ptr<SdkConnection::IncomingCall>& call) {
      if (call->Op() != Operation::Handshake) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host requires a handshake before SDK operations");
      }

      const auto& payload = call->Payload();
      if (payload.at("protocol").get<uint32_t>() != SdkConnection::kProtocolVersion ||
          payload.at("profile").get<std::string>() != host.profile_key) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host runtime/profile is incompatible");
      }

      const ProcessIdentity owner_process{payload.at("process").at("pid").get<uint64_t>(),
                                          payload.at("process").at("generation").get<uint64_t>()};
      if (owner_process.pid != identity.process_id ||
          (identity.generation && owner_process.generation != *identity.generation)) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host handshake does not match the authenticated peer");
      }

      const bool acquire = payload.at("owner").get<bool>();
      bool attached = false;
      {
        std::lock_guard lock(mutex);
        if (closed) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host connection is closed");
        }

        watch = std::make_unique<ProcessWatch>(owner_process);
        objects = std::make_shared<HostObjects>(host.incarnation, generation, host.manager);
        owner = acquire && host.lifetime->AddOwner(generation, host.Now());
        attached = owner;
      }

      Json reply{{"protocol", SdkConnection::kProtocolVersion},
                 {"profile", host.profile_key},
                 {"incarnation", host.incarnation},
                 {"connection", generation},
                 {"process", {{"pid", host.process.pid}, {"generation", host.process.generation}}},
                 {"owner", attached}};
      call->Complete({acquire && !attached ? static_cast<int32_t>(FOUNDRY_LOCAL_ERROR_INVALID_USAGE) : 0,
                      std::move(reply), acquire && !attached ? "native host shutdown is committed" : ""});
    }

    void Execute(const std::shared_ptr<SdkConnection::IncomingCall>& call) {
      if (call->IsCancelled()) {
        call->Complete({FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, nullptr, "SDK invocation cancelled before execution"});
      } else if (call->Op() == Operation::Observe) {
        const auto snapshot = host.lifetime->Poll(host.Now());
        call->Complete({0, {{"state", static_cast<int>(snapshot.state)}, {"owners", snapshot.owner_count}}, {}});
      } else if (ManagerDispatcher::Handles(call->Op())) {
        host.managers.Dispatch(call, IsOwner() ? std::optional{generation} : std::nullopt);
      } else if (ModelDispatcher::Handles(call->Op())) {
        host.models.Dispatch(call, *objects);
      } else if (SessionDispatcher::Handles(call->Op())) {
        SessionDispatcher(*objects, *connection).Dispatch(call);
      } else {
        call->Complete({FOUNDRY_LOCAL_ERROR_INVALID_USAGE, nullptr, "native host handshake cannot be repeated"});
      }
    }

    bool IsOwner() const {
      std::lock_guard lock(mutex);
      return owner && !closed;
    }

    void Run() noexcept {
      std::vector<Worker> workers;
      try {
        auto handshake = connection->Accept();
        bool authenticated = false;
        if (handshake) {
          try {
            Handshake(handshake);
            authenticated = true;
          } catch (const Exception& error) {
            handshake->Complete({static_cast<int32_t>(error.code()), nullptr, error.what()});
          } catch (const Json::exception&) {
            handshake->Complete({FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, nullptr, "invalid native host handshake"});
          }
        }

        while (handshake) {
          auto call = connection->Accept();
          if (!call) {
            break;
          }
          if (!authenticated) {
            call->Complete({FOUNDRY_LOCAL_ERROR_INVALID_USAGE, nullptr, "native host handshake was rejected"});
            continue;
          }

          std::erase_if(workers, [](Worker& worker) {
            if (!worker.done->load(std::memory_order_acquire)) {
              return false;
            }

            worker.thread.join();
            return true;
          });
          HostLifetime::WorkGuard admission;
          bool permitted = false;
          {
            std::lock_guard lock(mutex);
            if (!closed && owner) {
              admission = host.lifetime->TryAdmitIpcWork(generation, host.Now());
              permitted = static_cast<bool>(admission);
            } else if (!closed && IsObservation(call->Op())) {
              permitted = host.lifetime->Poll(host.Now()).state != HostLifetime::State::kShutdownCommitted;
            }
          }

          if (!permitted) {
            call->Complete({FOUNDRY_LOCAL_ERROR_INVALID_USAGE, nullptr, "SDK operation requires an active host owner"});
            continue;
          }

          if (IsResourceRelease(call->Op())) {
            Execute(call);
            continue;
          }

          if (host.active_calls.fetch_add(1, std::memory_order_acq_rel) >= kMaxConcurrentCalls) {
            host.active_calls.fetch_sub(1, std::memory_order_acq_rel);
            call->Complete({FOUNDRY_LOCAL_ERROR_INVALID_USAGE, nullptr, "shared host is busy; retry later"});
            continue;
          }

          ScopeGuard release_slot([&]() noexcept { host.active_calls.fetch_sub(1, std::memory_order_acq_rel); });
          workers.emplace_back();
          auto& worker = workers.back();
          worker.done = std::make_shared<std::atomic<bool>>(false);
          worker.thread = std::thread([this, call, done = worker.done, admission = std::move(admission)]() mutable {
            ScopeGuard finished([&]() noexcept {
              admission.Complete(host.Now());
              host.active_calls.fetch_sub(1, std::memory_order_acq_rel);
              done->store(true, std::memory_order_release);
            });
            try {
              Execute(call);
            } catch (...) {
              Close();
            }
          });
          release_slot.Dismiss();
        }
      } catch (...) {
        // A malformed handshake, dead peer or transport loss invalidates only this connection.
      }

      Close();
      for (auto& worker : workers) {
        if (worker.thread.joinable()) {
          worker.thread.join();
        }
      }

      finished.store(true, std::memory_order_release);
    }

    Impl& host;
    const PeerIdentity identity;
    const uint64_t generation;
    const HostLifetime::TimePoint handshake_deadline;
    const std::unique_ptr<SdkConnection> connection;
    mutable std::mutex mutex;
    std::unique_ptr<ProcessWatch> watch;
    std::shared_ptr<HostObjects> objects;
    bool owner = false;
    bool closed = false;
    std::atomic<bool> finished{false};
    std::thread thread;
  };

  Impl(std::shared_ptr<api::ManagerBackend> manager_in, const std::filesystem::path& endpoint,
       std::string profile_key_in, std::string incarnation_in, ProcessIdentity starter,
       std::shared_ptr<HostLifetime> lifetime_in, Clock clock_in)
      : manager(std::move(manager_in)),
        profile_key(std::move(profile_key_in)),
        incarnation(std::move(incarnation_in)),
        process(CurrentProcessIdentity()),
        clock(std::move(clock_in)),
        starter_watch(starter),
        lifetime(std::move(lifetime_in)),
        models(manager),
        managers(manager, RequireLifetime(lifetime)),
        listener(endpoint) {
    if (!clock || profile_key.empty() || incarnation.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host requires clock, profile and incarnation");
    }
  }

  ~Impl() {
    RequestStop();
    if (monitor.joinable()) {
      monitor.join();
    } else {
      Drain();
    }
  }

  HostLifetime::TimePoint Now() const noexcept { return clock(); }

  void Start() {
    acceptor = std::thread([this] {
      try {
        while (!stopping.load(std::memory_order_acquire)) {
          auto [channel, identity] = listener.Accept();
          std::lock_guard lock(mutex);
          if (stopping.load(std::memory_order_acquire)) {
            break;
          }
          if (peers.size() >= kMaxConnections) {
            continue;
          }
          if (next_generation == std::numeric_limits<uint64_t>::max()) {
            FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "native host connection generation exhausted");
          }

          auto peer = std::make_shared<Peer>(*this, std::move(channel), identity, next_generation++);
          peers.push_back(peer);
          peer->thread = std::thread([value = peer.get()] { value->Run(); });
        }
      } catch (...) {
        if (!stopping.load(std::memory_order_acquire)) {
          Fail(std::current_exception());
          RequestStop();
        }
      }
    });
    monitor = std::thread([this] {
      try {
        while (!stopping.load(std::memory_order_acquire)) {
          const auto now = Now();
          const auto snapshot = lifetime->Poll(now);
          if (snapshot.state == HostLifetime::State::kShutdownCommitted ||
              (snapshot.state == HostLifetime::State::kAwaitingFirstOwner && starter_watch.IsExited() &&
               lifetime->RetireUnusedStartup(now))) {
            break;
          }

          auto observed = Peers();
          for (const auto& peer : observed) {
            try {
              if (peer->MustClose(now)) {
                peer->Close();
              }
            } catch (...) {
              // An uncertain process watch fails the connection, never masquerades as a confirmed process exit.
              peer->Close();
            }
          }

          observed.clear();
          Reap();

          std::unique_lock lock(mutex);
          wake.wait_for(lock, 20ms, [this] { return stopping.load(std::memory_order_acquire); });
        }
      } catch (...) {
        Fail(std::current_exception());
      }

      RequestStop();
      Drain();
      try {
        while (true) {
          const auto snapshot = lifetime->Poll(Now());
          if (snapshot.ipc_work_count == 0 && snapshot.http_work_count == 0) {
            break;
          }

          std::this_thread::sleep_for(20ms);
        }

        manager->Shutdown();
      } catch (...) {
        Fail(std::current_exception());
      }

      {
        std::lock_guard lock(mutex);
        stopped = true;
      }

      wake.notify_all();
    });
  }

  void RequestStop() noexcept {
    stopping.store(true, std::memory_order_release);
    listener.Close();
    wake.notify_all();
  }

  std::vector<std::shared_ptr<Peer>> Peers() {
    std::lock_guard lock(mutex);
    return peers;
  }

  void Reap() {
    std::vector<std::shared_ptr<Peer>> retired;
    {
      std::lock_guard lock(mutex);
      auto peer = peers.begin();
      while (peer != peers.end()) {
        if ((*peer)->finished.load(std::memory_order_acquire)) {
          retired.push_back(std::move(*peer));
          peer = peers.erase(peer);
        } else {
          ++peer;
        }
      }
    }
  }

  void Drain() noexcept {
    if (acceptor.joinable()) {
      acceptor.join();
    }

    auto retired = Peers();
    for (const auto& peer : retired) {
      peer->Close();
    }
    for (const auto& peer : retired) {
      if (peer->thread.joinable()) {
        peer->thread.join();
      }
    }

    std::lock_guard lock(mutex);
    peers.clear();
  }

  void Fail(std::exception_ptr error) noexcept {
    std::lock_guard lock(mutex);
    if (!failure) {
      failure = std::move(error);
    }
  }

  const std::shared_ptr<api::ManagerBackend> manager;
  const std::string profile_key;
  const std::string incarnation;
  const ProcessIdentity process;
  const Clock clock;
  ProcessWatch starter_watch;
  const std::shared_ptr<HostLifetime> lifetime;
  ModelDispatcher models;
  ManagerDispatcher managers;
  Listener listener;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::exception_ptr failure;
  std::vector<std::shared_ptr<Peer>> peers;
  uint64_t next_generation = 1;
  std::atomic<bool> stopping{false};
  std::atomic<std::size_t> active_calls{0};
  bool stopped = false;
  std::thread acceptor;
  std::thread monitor;
};

HostServer::HostServer(std::shared_ptr<api::ManagerBackend> manager, const std::filesystem::path& endpoint,
                       std::string profile_key, std::string incarnation, ProcessIdentity starter,
                       std::shared_ptr<HostLifetime> lifetime, Clock clock)
    : impl_(std::make_unique<Impl>(std::move(manager), endpoint, std::move(profile_key), std::move(incarnation),
                                   starter, std::move(lifetime), std::move(clock))) {
  impl_->Start();
}

HostServer::~HostServer() = default;

HostLifetime& HostServer::Lifetime() noexcept { return *impl_->lifetime; }

bool HostServer::WaitUntilStopped(HostLifetime::TimePoint deadline) {
  std::unique_lock lock(impl_->mutex);
  return impl_->wake.wait_until(lock, deadline, [this] { return impl_->stopped; });
}

void HostServer::RethrowFailure() const {
  std::lock_guard lock(impl_->mutex);
  if (impl_->failure) {
    std::rethrow_exception(impl_->failure);
  }
}

}  // namespace fl::hosting
