// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

namespace fl {

/// Work that supports cancellation from another thread.
/// Cancel() must be thread-safe and idempotent while registered, and interrupt an in-progress engine call.
class ICancellable {
 public:
  virtual ~ICancellable() = default;

  virtual void Cancel() = 0;
};

}  // namespace fl
