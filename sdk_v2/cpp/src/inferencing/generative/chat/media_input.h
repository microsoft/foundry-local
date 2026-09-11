// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/session/request.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/message_item.h"

#include <vector>

namespace fl {

/// Media parts referenced by a request, plus copies of the messages that carry them.
///
/// The media prompt path (OnnxChatGenerator::CreateWithMedia) renders these messages and nothing else, so it cannot
/// use the transcript projection and has no way to render a tool call or a tool result.
///
/// Lifetime: `images` and `audios` are **borrowed** from the Request that CollectMediaInput was given — they point
/// into the items that request owns. A MediaInput must not outlive that request, and the bytes must not be captured
/// beyond the turn that reads them. `messages` are copies, so they carry no such constraint.
struct MediaInput {
  std::vector<MessageItem> messages;
  std::vector<const ImageItem*> images;
  std::vector<const AudioItem*> audios;

  bool Empty() const { return images.empty() && audios.empty(); }
};

/// Collect the request's media parts and the messages carrying them. Returns an empty MediaInput (no messages
/// either) when the request has no media at all.
///
/// The returned image and audio pointers borrow from `request` — see MediaInput's lifetime note.
MediaInput CollectMediaInput(const Request& request);

/// Everything outside the request's own items that decides whether a media turn is allowed.
struct MediaTurnContext {
  /// The session transcript already holds a conversation.
  bool session_has_history = false;
  /// This turn declares tools, so the model may answer it with a call.
  bool tools_declared = false;
};

/// Reject a media turn this runtime cannot serve.
///
/// Media is single-shot: image and audio bytes go straight to the generator and never enter the transcript, so no
/// later prompt can reproduce them. That gives one conversation-scoped rule which does not depend on whether the
/// session happened to still be cached:
///
///  - Media is only allowed while the conversation has no history. Prior history reaches a turn two ways — the live
///    transcript, or a chain replayed into this request's inputs after the session cache dropped it — and both are
///    rejected identically, so a continuation behaves the same warm and cold.
///  - A media turn must not declare tools. The model would be free to answer with a call, and the result for that
///    call would arrive on a later turn that can no longer show the model the image it was looking at. Rejecting up
///    front is the only way that follow-up is never stranded.
///
/// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT — the caller sent a request this runtime cannot
///         serve, so it maps to HTTP 400. No-op when the request carries no media.
void ValidateMediaTurn(const MediaInput& media,
                       const std::vector<TranscriptMessage>& inputs,
                       const MediaTurnContext& context);

}  // namespace fl
