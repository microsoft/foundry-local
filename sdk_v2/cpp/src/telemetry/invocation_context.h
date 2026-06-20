// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>
#include <utility>

namespace fl {

/// Generate an RFC 4122 v4 UUID, hex-encoded with hyphens. Not a cryptographic
/// identifier — used for per-operation correlation and per-process session ids.
std::string MakeGuidV4Hex();

/// Per-operation telemetry context, threaded from an entry point through every
/// action it triggers.
///
///  - `user_agent`    identifies the calling client (SDK, CLI, Node, browser…).
///  - `correlation_id` groups every event emitted while servicing one logical
///                     operation, so the route action, the inference it drives,
///                     the Model metrics event and any error can be joined.
///  - `indirect`      is true when this action happened as a consequence of
///                     another action rather than a direct user/API call (e.g. a
///                     session driven by an HTTP route, or a per-provider EP
///                     download under an overall attempt).
struct InvocationContext {
  std::string user_agent;
  std::string correlation_id;
  bool indirect = false;

  /// A direct, top-level context with a freshly generated correlation id.
  static InvocationContext Direct(std::string user_agent = "") {
    InvocationContext ctx;
    ctx.user_agent = std::move(user_agent);
    ctx.correlation_id = MakeGuidV4Hex();
    ctx.indirect = false;
    return ctx;
  }

  /// Derive a context for an action triggered by this one: same correlation id
  /// and user agent, but marked indirect.
  InvocationContext AsIndirect() const {
    InvocationContext ctx = *this;
    ctx.indirect = true;
    return ctx;
  }

  /// Guarantee a correlation id is present, generating one when empty. Lets an
  /// entry point that received a default-constructed context still group its
  /// events (e.g. an SDK caller that didn't build a Direct() context).
  InvocationContext& EnsureCorrelationId() {
    if (correlation_id.empty()) {
      correlation_id = MakeGuidV4Hex();
    }
    return *this;
  }
};

}  // namespace fl
