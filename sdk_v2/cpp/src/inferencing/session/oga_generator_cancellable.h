// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellable.h"
#include "inferencing/session/operation_context.h"

#include <atomic>
#include <optional>
#include <stdexcept>
#include <type_traits>

struct OgaGenerator;

namespace fl {

/// Adapts a raw OgaGenerator to ICancellable.
///
/// Some paths (embeddings, streaming PCM transcription, Nemotron decode) drive an OgaGenerator
/// directly instead of going through OnnxAudioGenerator, so they have no Cancel() of
/// their own. Wrapping the generator lets an operation publish it for cancellation, which
/// is what makes those loops interruptible mid-compute rather than only between tokens.
///
/// Non-owning: the wrapped generator must outlive this adapter.
class OgaGeneratorCancellable : public ICancellable {
 public:
  explicit OgaGeneratorCancellable(OgaGenerator& generator) : generator_(generator) {}

  bool Cancel() noexcept override;

 private:
  OgaGenerator& generator_;
};

namespace detail {

template <typename Generator>
[[nodiscard]] bool IsOperationTerminationConfirmed(Generator& generator, const OperationContext& operation) {
  return operation.ShouldStop() &&
         (operation.EngineCancelDelivered() || generator.IsSessionTerminated());
}

template <typename Generator>
[[nodiscard]] bool IsGeneratorTerminationConfirmed(
    Generator& generator, const std::atomic<bool>& engine_termination_delivered) {
  return engine_termination_delivered.load(std::memory_order_acquire) ||
         generator.IsSessionTerminated();
}

template <typename Action, typename TerminationConfirmed>
[[nodiscard]] bool TryGeneratorVoidCall(const Action& action,
                                        const TerminationConfirmed& termination_confirmed) {
  try {
    action();
    return true;
  } catch (const std::runtime_error&) {
    if (termination_confirmed()) {
      return false;
    }

    throw;
  }
}

template <typename Result, typename Action, typename TerminationConfirmed>
[[nodiscard]] std::optional<Result> TryGeneratorValueCall(
    const Action& action, const TerminationConfirmed& termination_confirmed) {
  try {
    return std::optional<Result>{action()};
  } catch (const std::runtime_error&) {
    if (termination_confirmed()) {
      return std::nullopt;
    }

    throw;
  }
}

template <typename Generator, typename CancellationConfirmed>
[[nodiscard]] bool IsGeneratorDoneCancellationSafeImpl(
    Generator& generator, const CancellationConfirmed& cancellation_confirmed) {
  // IsDone may throw after terminate_session lands, so query termination before it and re-check the exact
  // evidence if cancellation lands between these calls.
  if (generator.IsSessionTerminated() || cancellation_confirmed()) {
    return true;
  }

  try {
    return generator.IsDone();
  } catch (const std::runtime_error&) {
    if (cancellation_confirmed()) {
      return true;
    }

    throw;
  }
}

}  // namespace detail

/// Query an OGA generator owned by a concrete wrapper that records exact successful terminate_session delivery.
///
/// A throwing SetRuntimeOption call can still leave the pinned OGA session terminated, so both evidence sources
/// are checked before IsDone and again if IsDone raises std::runtime_error. Cancellation intent alone is never
/// sufficient to suppress an unrelated runtime failure.
template <typename Generator>
[[nodiscard]] bool IsGeneratorDoneCancellationSafe(
    Generator& generator, const std::atomic<bool>& engine_termination_delivered) {
  return detail::IsGeneratorDoneCancellationSafeImpl(generator, [&] {
    return detail::IsGeneratorTerminationConfirmed(generator, engine_termination_delivered);
  });
}

/// Query a raw OGA generator driven by one exact operation.
///
/// Natural termination is checked first. A runtime_error is suppressed only when this operation is stopped and
/// either its engine cancellation was delivered successfully or this generator confirms session termination.
template <typename Generator>
[[nodiscard]] bool IsGeneratorDoneCancellationSafe(Generator& generator,
                                                    const OperationContext& operation) {
  if (operation.ShouldStop()) {
    return true;
  }

  return detail::IsGeneratorDoneCancellationSafeImpl(
      generator, [&] { return detail::IsOperationTerminationConfirmed(generator, operation); });
}

/// Generate one token with a raw OGA generator, tolerating exactly one expected failure: the
/// std::runtime_error ORT GenAI raises when terminate_session interrupts an in-flight call.
///
/// The suppression is deliberately narrow:
///   1. *this* operation has a latched stop (not merely "some cancellation raced somewhere"),
///   2. either an engine cancellation was delivered successfully to this operation or this generator itself
///      reports that its session terminated.
/// Anything else — a genuine engine failure that happens to coincide with a cancel — propagates unchanged.
///
/// Templated so OgaGenerator can stay forward-declared and the behaviour is testable without a model.
template <typename Generator>
[[nodiscard]] bool TryGenerateNextToken(Generator& generator, const OperationContext& operation) {
  if (operation.ShouldStop()) {
    return false;
  }

  return detail::TryGeneratorVoidCall(
      [&] { generator.GenerateNextToken(); },
      [&] { return detail::IsOperationTerminationConfirmed(generator, operation); });
}

/// Generate one token through a concrete wrapper. Cancellation intent is deliberately not consulted.
template <typename Generator>
[[nodiscard]] bool TryGenerateNextToken(
    Generator& generator, const std::atomic<bool>& engine_termination_delivered) {
  return detail::TryGeneratorVoidCall(
      [&] { generator.GenerateNextToken(); },
      [&] { return detail::IsGeneratorTerminationConfirmed(generator, engine_termination_delivered); });
}

/// Set inputs on a raw OGA generator published by one exact operation.
template <typename Generator, typename Tensors>
[[nodiscard]] bool TrySetGeneratorInputs(Generator& generator, Tensors& tensors,
                                         const OperationContext& operation) {
  if (operation.ShouldStop()) {
    return false;
  }

  return detail::TryGeneratorVoidCall(
      [&] { generator.SetInputs(tensors); },
      [&] { return detail::IsOperationTerminationConfirmed(generator, operation); });
}

/// Read next tokens from a raw OGA generator published by one exact operation.
template <typename Generator>
[[nodiscard]] auto TryGetNextTokens(Generator& generator, const OperationContext& operation)
    -> std::optional<std::remove_cvref_t<decltype(generator.GetNextTokens())>> {
  using NextTokens = std::remove_cvref_t<decltype(generator.GetNextTokens())>;

  if (operation.ShouldStop()) {
    return std::nullopt;
  }

  return detail::TryGeneratorValueCall<NextTokens>(
      [&] { return generator.GetNextTokens(); },
      [&] { return detail::IsOperationTerminationConfirmed(generator, operation); });
}

/// Decode one token only while the exact raw-generator operation is still running. Tokenizer decoding is not
/// an engine-termination call, so unrelated failures propagate unchanged.
template <typename TokenizerStream, typename Token>
[[nodiscard]] auto TryDecodeToken(TokenizerStream& stream, Token token, const OperationContext& operation)
    -> std::optional<std::remove_cvref_t<decltype(stream.Decode(token))>> {
  using Decoded = std::remove_cvref_t<decltype(stream.Decode(token))>;

  if (operation.ShouldStop()) {
    return std::nullopt;
  }

  return std::optional<Decoded>{stream.Decode(token)};
}

/// Account for one raw-generator token only while its exact operation is still running.
[[nodiscard]] inline bool TryIncrementTokenCount(int& token_count, const OperationContext& operation) noexcept {
  if (operation.ShouldStop()) {
    return false;
  }

  ++token_count;
  return true;
}

/// Read next tokens through a concrete wrapper. Cancellation intent is deliberately not consulted.
template <typename Generator>
[[nodiscard]] auto TryGetNextTokens(
    Generator& generator, const std::atomic<bool>& engine_termination_delivered)
    -> std::optional<std::remove_cvref_t<decltype(generator.GetNextTokens())>> {
  using NextTokens = std::remove_cvref_t<decltype(generator.GetNextTokens())>;
  return detail::TryGeneratorValueCall<NextTokens>(
      [&] { return generator.GetNextTokens(); },
      [&] { return detail::IsGeneratorTerminationConfirmed(generator, engine_termination_delivered); });
}

}  // namespace fl
