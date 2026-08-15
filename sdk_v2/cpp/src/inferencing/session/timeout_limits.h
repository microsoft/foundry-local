// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellation_state.h"

#include <chrono>

namespace fl {

// Leave half the clock range as headroom when converting the relative timeout to an absolute deadline.
inline constexpr std::chrono::milliseconds kMaxSupportedRequestTimeout =
    std::chrono::duration_cast<std::chrono::milliseconds>(CancellationState::Clock::duration::max() / 2);

}  // namespace fl
