// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_objects.h"

#include "exception.h"

#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace fl::hosting {
namespace {

uint64_t ObjectInteger(const nlohmann::json& value) {
  if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid shared-host object identity");
  }

  const auto integer = value.get<uint64_t>();
  if (integer == 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid shared-host object identity");
  }

  return integer;
}

bool ValidIncarnation(const std::string& value) { return !value.empty() && value.find('\0') == std::string::npos; }

}  // namespace

nlohmann::json EncodeObjectIdentity(const RemoteObjectIdentity& identity) {
  return {{"incarnation", identity.incarnation},
          {"connection", identity.connection_generation},
          {"object", identity.object}};
}

RemoteObjectIdentity DecodeObjectIdentity(const nlohmann::json& value) {
  try {
    RemoteObjectIdentity identity{value.at("incarnation").get<std::string>(), ObjectInteger(value.at("connection")),
                                  ObjectInteger(value.at("object"))};
    if (!ValidIncarnation(identity.incarnation)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid shared-host incarnation");
    }

    return identity;
  } catch (const nlohmann::json::exception&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid shared-host object identity");
  }
}

struct HostObjects::State {
  const std::string incarnation;
  const uint64_t connection;
  const std::shared_ptr<api::ManagerBackend> manager;
  mutable std::mutex mutex;
  bool closed = false;
  uint64_t next_object = 1;
  std::map<uint64_t, std::shared_ptr<SessionResource>> sessions;
  std::map<uint64_t, std::shared_ptr<PreflightResource>> preflights;

  State(std::string incarnation_in, uint64_t connection_in, std::shared_ptr<api::ManagerBackend> manager_in)
      : incarnation(std::move(incarnation_in)), connection(connection_in), manager(std::move(manager_in)) {}

  void RequireOpen() const {
    if (closed) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "shared-host connection is closed");
    }
  }

  void Validate(const RemoteObjectIdentity& identity) const {
    RequireOpen();
    if (identity.incarnation != incarnation || identity.connection_generation != connection || identity.object == 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "object does not belong to this shared-host connection");
    }
  }

  RemoteObjectIdentity Allocate() {
    RequireOpen();
    if (next_object == std::numeric_limits<uint64_t>::max()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "shared-host object identity space is exhausted");
    }

    return {incarnation, connection, next_object++};
  }
};

HostObjects::SessionResource::SessionResource(std::shared_ptr<api::ManagerBackend> manager_in,
                                              std::unique_ptr<api::SessionBackend> session_in)
    : manager(std::move(manager_in)), implementation(std::move(session_in)) {}

void HostObjects::SessionResource::AdmitInvocation(uint64_t id, std::function<void()> cancel) {
  if (id == 0 || !cancel) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK session invocation");
  }

  auto next = std::make_shared<std::function<void()>>(std::move(cancel));
  bool retired;
  {
    std::lock_guard lock(cancellation_mutex_);
    retired = retired_.load(std::memory_order_acquire);
    if (!retired && !cancellations_.emplace(id, next).second) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "SDK session invocation is already admitted");
    }
  }

  if (retired) {
    (*next)();
  }
}

void HostObjects::SessionResource::CompleteInvocation(uint64_t id) noexcept {
  decltype(cancellations_)::node_type retired;
  {
    std::lock_guard lock(cancellation_mutex_);
    retired = cancellations_.extract(id);
  }
}

bool HostObjects::SessionResource::HasInvocation(uint64_t id) const {
  std::lock_guard lock(cancellation_mutex_);
  return cancellations_.contains(id);
}

void HostObjects::SessionResource::Retire() noexcept {
  decltype(cancellations_) cancellations;
  {
    std::lock_guard lock(cancellation_mutex_);
    if (retired_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    cancellations_.swap(cancellations);
  }

  for (const auto& [id, cancel] : cancellations) {
    try {
      (*cancel)();
    } catch (...) {
      // Cancellation hooks cannot unwind disposal. The execution worker retains ownership until it finishes.
    }
  }
}

HostObjects::HostObjects(std::string incarnation, uint64_t connection_generation,
                         std::shared_ptr<api::ManagerBackend> manager)
    : state_(std::make_unique<State>(std::move(incarnation), connection_generation, std::move(manager))) {
  if (!ValidIncarnation(state_->incarnation) || state_->connection == 0 || !state_->manager ||
      state_->manager->Kind() != api::BackendKind::Embedded) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "shared-host objects require an embedded manager and connection identity");
  }
}

HostObjects::~HostObjects() { Close(); }

RemoteObjectIdentity HostObjects::AddSession(std::unique_ptr<api::SessionBackend> session) {
  if (!session || session->Kind() != api::BackendKind::Embedded) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host session must use the embedded backend");
  }

  auto resource = std::make_shared<SessionResource>(state_->manager, std::move(session));
  std::lock_guard lock(state_->mutex);
  const auto identity = state_->Allocate();
  state_->sessions.emplace(identity.object, std::move(resource));
  return identity;
}

RemoteObjectIdentity HostObjects::AddPreflight(std::unique_ptr<api::PreflightBackend> preflight) {
  if (!preflight || preflight->Kind() != api::BackendKind::Embedded) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host preflight must use the embedded backend");
  }

  auto resource = std::make_shared<PreflightResource>(state_->manager, std::move(preflight));
  std::lock_guard lock(state_->mutex);
  const auto identity = state_->Allocate();
  state_->preflights.emplace(identity.object, std::move(resource));
  return identity;
}

std::shared_ptr<HostObjects::SessionResource> HostObjects::Session(const RemoteObjectIdentity& identity) const {
  std::lock_guard lock(state_->mutex);
  state_->Validate(identity);
  const auto found = state_->sessions.find(identity.object);
  if (found == state_->sessions.end()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "unknown or released shared-host session");
  }

  return found->second;
}

std::shared_ptr<HostObjects::PreflightResource> HostObjects::Preflight(const RemoteObjectIdentity& identity) const {
  std::lock_guard lock(state_->mutex);
  state_->Validate(identity);
  const auto found = state_->preflights.find(identity.object);
  if (found == state_->preflights.end()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "unknown or released shared-host preflight");
  }

  return found->second;
}

void HostObjects::ReleaseSession(const RemoteObjectIdentity& identity) {
  decltype(state_->sessions)::node_type retired;
  {
    std::lock_guard lock(state_->mutex);
    state_->Validate(identity);
    retired = state_->sessions.extract(identity.object);
    if (retired.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "unknown or released shared-host session");
    }
  }

  retired.mapped()->Retire();
}

void HostObjects::ReleasePreflight(const RemoteObjectIdentity& identity) {
  decltype(state_->preflights)::node_type retired;
  {
    std::lock_guard lock(state_->mutex);
    state_->Validate(identity);
    retired = state_->preflights.extract(identity.object);
    if (retired.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "unknown or released shared-host preflight");
    }
  }
}

void HostObjects::Close() noexcept {
  decltype(state_->sessions) sessions;
  decltype(state_->preflights) preflights;
  {
    std::lock_guard lock(state_->mutex);
    state_->closed = true;
    state_->sessions.swap(sessions);
    state_->preflights.swap(preflights);
  }

  for (const auto& [id, session] : sessions) {
    session->Retire();
  }
}

}  // namespace fl::hosting
