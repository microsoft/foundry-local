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

/// Media parts and the messages that carry them.
///
/// The media prompt path renders these messages and has no way to render a tool call or a tool result.
///
/// The pointers borrow from the request or transcript supplied to CollectMediaInput. Transcript-derived messages
/// also borrow their media parts, so the MediaInput must not outlive that transcript.
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

/// Project the complete transcript into media messages and buffers in conversation order.
MediaInput CollectMediaInput(const std::vector<TranscriptMessage>& messages);

/// Check request media without copying its contents.
bool RequestHasMedia(const Request& request);

/// Everything outside the request's own items that decides whether a media turn is allowed.
struct MediaTurnContext {
  /// This turn declares tools, so the model may answer it with a call.
  bool tools_declared = false;
  /// The model's vision path cannot encode more than one image in a prompt.
  bool single_image_only = false;
};

/// Reject tool declarations before their schemas are admitted on a media turn.
void ValidateMediaToolDefinitions(bool tools_declared);

/// Reject media combined with declared tools or historical tool activity, which the media template cannot render,
/// and more than one image when the context limits the prompt to a single image.
///
/// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT — the caller sent a request this runtime cannot
///         serve, so it maps to HTTP 400. No-op when the request carries no media.
void ValidateMediaTurn(const MediaInput& media,
                       const std::vector<TranscriptMessage>& inputs,
                       const MediaTurnContext& context);

}  // namespace fl
