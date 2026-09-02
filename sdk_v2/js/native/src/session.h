// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Napi::ObjectWrap<ChatSession> over foundry_local::ChatSession.
//
// Surface:
//   * new ChatSession(model) — sync construction; underlying
//     flSession_Create is fast.
//   * session.processRequest(request) -> Promise<Response>  (PromiseWorker<Response>)
//   * session.processStreamingRequest(request, onItem) -> Promise<Response> — streaming bridge via
//     Napi::ThreadSafeFunction; resolves with the terminal Response after every item callback drains.
//     The JS layer wraps this in an AsyncIterable whose `.response` promise carries the resolved value.
//   * session.setOptions(kvp) — session-level options applied to subsequent sends.
//   * ChatSession adds: turnCount(), undoTurns(count), addToolDefinition({...}).
//   * dispose() — drops the underlying flSession; subsequent calls reject with
//     a FoundryLocalError tagged INVALID_USAGE.
//
// The abstract `Session` base in the TS layer is not represented by its own
// ObjectWrap — modality-specific session classes (`ChatSession` today,
// `AudioSession` / `EmbeddingsSession` later) each get their own ObjectWrap.
//
// Lifetime: the ChatSession pins the parent Manager via an ObjectReference so
// the underlying foundry_local::Model the C++ Session captured can't be
// released out from under it.
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <memory>

namespace foundry_local_node {

class ChatSession : public Napi::ObjectWrap<ChatSession> {
 public:
  static Napi::Function Init(Napi::Env env);

  explicit ChatSession(const Napi::CallbackInfo& info);

 private:
  Napi::Value ProcessRequest(const Napi::CallbackInfo& info);
  Napi::Value ProcessStreamingRequest(const Napi::CallbackInfo& info);
  Napi::Value SetOptions(const Napi::CallbackInfo& info);
  Napi::Value AddToolDefinition(const Napi::CallbackInfo& info);
  Napi::Value RemoveToolDefinition(const Napi::CallbackInfo& info);
  Napi::Value TurnCount(const Napi::CallbackInfo& info);
  Napi::Value UndoTurns(const Napi::CallbackInfo& info);
  Napi::Value Dispose(const Napi::CallbackInfo& info);
  Napi::Value IsDisposed(const Napi::CallbackInfo& info);

  bool ThrowIfDisposed(Napi::Env env);

  std::unique_ptr<foundry_local::ChatSession> impl_;
  Napi::ObjectReference manager_;
};

// Napi::ObjectWrap<EmbeddingsSession> over foundry_local::EmbeddingsSession.
//
// Surface (subset of ChatSession):
//   * new EmbeddingsSession(model) — task validation lives in the TS layer
//     (matches the C# / Python conventions) so we do not re-check it here.
//   * embeddingsSession.processRequest(request) -> Promise<Response>
//   * embeddingsSession.setOptions(kvp)
//   * dispose() / isDisposed()
//
// Embeddings is one-shot — no streaming, no turn history, no tools. We
// intentionally do NOT register `processStreamingRequest`; the TS base's
// streaming method will then throw a TypeError at the native boundary if
// invoked, which is the right behaviour for a non-streaming modality.
class EmbeddingsSession : public Napi::ObjectWrap<EmbeddingsSession> {
 public:
  static Napi::Function Init(Napi::Env env);

  explicit EmbeddingsSession(const Napi::CallbackInfo& info);

 private:
  Napi::Value ProcessRequest(const Napi::CallbackInfo& info);
  Napi::Value SetOptions(const Napi::CallbackInfo& info);
  Napi::Value Dispose(const Napi::CallbackInfo& info);
  Napi::Value IsDisposed(const Napi::CallbackInfo& info);

  bool ThrowIfDisposed(Napi::Env env);

  std::unique_ptr<foundry_local::EmbeddingsSession> impl_;
  Napi::ObjectReference manager_;
};

// Napi::ObjectWrap<AudioSession> over foundry_local::AudioSession.
//
// Surface (mirrors ChatSession's inference surface; no audio-specific
// methods exist in the C++ wrapper):
//   * new AudioSession(model) — task validation lives in the TS layer.
//   * audioSession.processRequest(request) -> Promise<Response>
//   * audioSession.processStreamingRequest(request, onItem) — live
//     transcription streams TextItems as they're produced.
//   * audioSession.setOptions(kvp)
//   * dispose() / isDisposed()
class AudioSession : public Napi::ObjectWrap<AudioSession> {
 public:
  static Napi::Function Init(Napi::Env env);

  explicit AudioSession(const Napi::CallbackInfo& info);

 private:
  Napi::Value ProcessRequest(const Napi::CallbackInfo& info);
  Napi::Value ProcessStreamingRequest(const Napi::CallbackInfo& info);
  Napi::Value SetOptions(const Napi::CallbackInfo& info);
  Napi::Value Dispose(const Napi::CallbackInfo& info);
  Napi::Value IsDisposed(const Napi::CallbackInfo& info);

  bool ThrowIfDisposed(Napi::Env env);

  std::unique_ptr<foundry_local::AudioSession> impl_;
  Napi::ObjectReference manager_;
};

}  // namespace foundry_local_node
