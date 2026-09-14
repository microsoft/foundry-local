// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "inferencing/session/types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fl {

/// Recognizes one complete, unfenced, line-anchored envelope while retaining at most 1 MiB.
///
/// This is intentionally not a grammar engine. It gives meaning only to the two exact marker
/// lines selected by trusted request conversion. Once one call completes, all later bytes are text.
class RawEnvelopeDetector {
 public:
  struct RejectedCandidate {
    std::string text;
  };

  using Event = std::variant<std::string, RejectedCandidate, ParsedToolCall>;

  struct Output {
    std::vector<Event> events;
  };

  static constexpr size_t kMaxBufferedBytes = 1024 * 1024;

  explicit RawEnvelopeDetector(RawEnvelopeDescriptor descriptor) : descriptor_(std::move(descriptor)) {}

  Output Push(std::string_view chunk) {
    Output output;
    if (chunk.empty()) {
      return output;
    }

    if (finished_) {
      EmitText(output, std::string(chunk));
      return output;
    }

    pending_.append(chunk);
    DrainCompleteLines(output);
    DrainSafeOutsideText(output);

    const bool pending_crlf_closer =
        inside_envelope_ && pending_.ends_with('\r') &&
        LineText(pending_) == descriptor_.end_marker;
    const auto buffered_pending_size =
        pending_.size() - static_cast<size_t>(pending_crlf_closer);
    if (buffered_pending_size > kMaxBufferedBytes ||
        candidate_.size() > kMaxBufferedBytes - buffered_pending_size) {
      EmitRejectedCandidate(output, std::move(candidate_));
      EmitRejectedCandidate(output, std::move(pending_));
      candidate_.clear();
      pending_.clear();
      inside_envelope_ = false;
      finished_ = true;
    }

    return output;
  }

  /// Finalizes a natural model EOS. A closing marker without a trailing newline is accepted only
  /// here; every other terminal cause must use Abort().
  Output FinalizeNatural() {
    Output output;
    if (finished_) {
      EmitText(output, std::move(pending_));
      pending_.clear();
      return output;
    }

    if (inside_envelope_ && pending_ == descriptor_.end_marker &&
        candidate_.size() + descriptor_.end_marker.size() <= kMaxBufferedBytes) {
      candidate_ += pending_;
      pending_.clear();
      EmitCall(output);
      return output;
    }

    if (inside_envelope_) {
      EmitRejectedCandidate(output, std::move(candidate_));
      EmitRejectedCandidate(output, std::move(pending_));
    } else {
      EmitText(output, std::move(pending_));
    }
    candidate_.clear();
    pending_.clear();
    inside_envelope_ = false;
    return output;
  }

  /// Rejects buffered candidate bytes for cancellation, limits, failures, stop sequences, and
  /// reasoning boundaries. Rejected bytes are tagged so composition never feeds them to the
  /// structured parser.
  Output Abort() {
    Output output;
    EmitRejectedCandidate(output, std::move(candidate_));
    EmitRejectedCandidate(output, std::move(pending_));
    candidate_.clear();
    pending_.clear();
    inside_envelope_ = false;
    fence_.reset();
    outside_line_decided_ = false;
    return output;
  }

  /// Rejects a candidate interrupted by reasoning without resetting the surrounding Markdown
  /// parser. The rejected bytes are visible output, so a trailing partial line also keeps the next
  /// visible bytes on that same line.
  Output RejectCandidateForReasoning() {
    Output output;
    const bool continues_line =
        LastByteContinuesLine(pending_.empty() ? candidate_ : pending_);
    EmitRejectedCandidate(output, std::move(candidate_));
    EmitRejectedCandidate(output, std::move(pending_));
    candidate_.clear();
    pending_.clear();
    inside_envelope_ = false;
    outside_line_decided_ = continues_line;
    return output;
  }

  Output DisableRecognition() {
    Output output;
    if (finished_) {
      return output;
    }

    EmitText(output, std::move(candidate_));
    EmitText(output, std::move(pending_));
    candidate_.clear();
    pending_.clear();
    inside_envelope_ = false;
    finished_ = true;
    return output;
  }

  bool InsideEnvelope() const noexcept {
    return inside_envelope_;
  }

  bool HasPendingCandidate() const noexcept {
    return inside_envelope_ || (!finished_ && !pending_.empty());
  }

  bool CallCompleted() const noexcept {
    return call_completed_;
  }

 private:
  static bool LastByteContinuesLine(std::string_view text) {
    return !text.empty() && text.back() != '\n';
  }

  static std::string_view LineText(std::string_view line) {
    if (!line.empty() && line.back() == '\n') {
      line.remove_suffix(1);
    }

    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    return line;
  }

  static void EmitText(Output& output, std::string text) {
    if (text.empty()) {
      return;
    }

    if (!output.events.empty()) {
      if (auto* previous = std::get_if<std::string>(&output.events.back())) {
        *previous += text;
        return;
      }
    }

    output.events.emplace_back(std::move(text));
  }

  static void EmitRejectedCandidate(Output& output, std::string text) {
    if (text.empty()) {
      return;
    }

    if (!output.events.empty()) {
      if (auto* previous = std::get_if<RejectedCandidate>(&output.events.back())) {
        previous->text += text;
        return;
      }
    }

    output.events.emplace_back(RejectedCandidate{std::move(text)});
  }

  void UpdateFence(std::string_view line) {
    auto marker = LineText(line);
    size_t indentation = 0;
    while (indentation < marker.size() && indentation < 3 && marker[indentation] == ' ') {
      ++indentation;
    }
    marker.remove_prefix(indentation);
    const auto fence_char = marker.empty() ? '\0' : marker.front();
    if (fence_char != '`' && fence_char != '~') {
      return;
    }

    size_t count = 0;
    while (count < marker.size() && marker[count] == fence_char) {
      ++count;
    }

    if (count < 3) {
      return;
    }

    if (!fence_.has_value()) {
      fence_ = std::string(marker.substr(0, count));
      return;
    }

    const auto suffix = marker.substr(count);
    if (fence_char == fence_->front() && count >= fence_->size() &&
        suffix.find_first_not_of(" \t") == std::string_view::npos) {
      fence_.reset();
    }
  }

  void EmitCall(Output& output) {
    ParsedToolCall call{GenerateToolCallId(), descriptor_.tool_name, std::move(candidate_)};
    call.argument_source = call.arguments;
    call.raw_envelope = true;
    output.events.emplace_back(std::move(call));
    candidate_.clear();
    inside_envelope_ = false;
    finished_ = true;
    call_completed_ = true;
  }

  void ProcessLine(Output& output, std::string line) {
    if (finished_) {
      EmitText(output, std::move(line));
      return;
    }

    const auto text = LineText(line);
    if (inside_envelope_) {
      if (text == descriptor_.end_marker) {
        if (candidate_.size() + text.size() > kMaxBufferedBytes) {
          EmitRejectedCandidate(output, std::move(candidate_));
          EmitRejectedCandidate(output, std::move(line));
          candidate_.clear();
          inside_envelope_ = false;
          finished_ = true;
          return;
        }

        const auto terminator_size = line.size() - text.size();
        candidate_.append(line.data(), text.size());
        EmitCall(output);
        EmitText(output, line.substr(text.size(), terminator_size));
        return;
      }

      if (candidate_.size() + line.size() > kMaxBufferedBytes) {
        EmitRejectedCandidate(output, std::move(candidate_));
        EmitRejectedCandidate(output, std::move(line));
        candidate_.clear();
        inside_envelope_ = false;
        finished_ = true;
        return;
      }

      candidate_ += line;
      return;
    }

    if (!fence_.has_value() && text == descriptor_.start_marker) {
      inside_envelope_ = true;
      candidate_ = std::move(line);
      return;
    }

    UpdateFence(line);
    EmitText(output, std::move(line));
  }

  void DrainCompleteLines(Output& output) {
    while (true) {
      const auto newline = pending_.find('\n');
      if (newline == std::string::npos) {
        break;
      }

      auto line = pending_.substr(0, newline + 1);
      pending_.erase(0, newline + 1);
      if (outside_line_decided_) {
        EmitText(output, std::move(line));
        outside_line_decided_ = false;
      } else {
        ProcessLine(output, std::move(line));
      }
    }
  }

  bool CouldStillAffectFence() {
    auto marker = std::string_view(pending_);
    size_t indentation = 0;
    while (indentation < marker.size() && indentation < 3 && marker[indentation] == ' ') {
      ++indentation;
    }

    if (indentation == marker.size()) {
      return true;
    }

    if (marker[indentation] == ' ') {
      return false;
    }

    marker.remove_prefix(indentation);
    const auto expected_fence_char = fence_.has_value() ? fence_->front() : marker.front();
    if (expected_fence_char != '`' && expected_fence_char != '~') {
      return false;
    }

    size_t count = 0;
    while (count < marker.size() && marker[count] == expected_fence_char) {
      ++count;
    }

    if (count == marker.size()) {
      return true;
    }

    const auto suffix = marker.substr(count);
    if (fence_.has_value()) {
      return count >= fence_->size() &&
             suffix.find_first_not_of(" \t") == std::string_view::npos;
    }

    if (count < 3) {
      return false;
    }

    fence_ = std::string(marker.substr(0, count));
    return false;
  }

  void DrainSafeOutsideText(Output& output) {
    if (pending_.empty()) {
      return;
    }

    if (finished_) {
      EmitText(output, std::move(pending_));
      pending_.clear();
      return;
    }

    if (inside_envelope_) {
      return;
    }

    if (outside_line_decided_) {
      EmitText(output, std::move(pending_));
      pending_.clear();
      return;
    }

    const auto marker = std::string_view(descriptor_.start_marker);
    const bool possible_start_marker =
        (pending_.size() <= marker.size() && marker.starts_with(pending_)) ||
        (pending_.size() == marker.size() + 1 && pending_.back() == '\r' &&
         std::string_view(pending_).starts_with(marker));
    if (possible_start_marker) {
      return;
    }

    if (CouldStillAffectFence()) {
      return;
    }

    EmitText(output, std::move(pending_));
    pending_.clear();
    outside_line_decided_ = true;
  }

  RawEnvelopeDescriptor descriptor_;
  std::string pending_;
  std::string candidate_;
  std::optional<std::string> fence_;
  bool inside_envelope_ = false;
  bool finished_ = false;
  bool call_completed_ = false;
  bool outside_line_decided_ = false;
};

}  // namespace fl
