// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

/// <summary>
/// An audio session for automatic-speech-recognition models.
/// Validates the model task at construction time.
/// </summary>
public sealed class AudioSession : Session
{
    /// <summary>
    /// Create an audio session from a loaded speech-recognition model.
    /// Each request produces a <see cref="SpeechResultItem"/> with the full transcript and per-segment detail;
    /// the streaming callback (when registered) receives a <see cref="SpeechSegmentItem"/> per decoded token.
    /// </summary>
    /// <param name="model">A loaded model whose task is "automatic-speech-recognition".</param>
    /// <exception cref="ArgumentException">If the model's task is not automatic-speech-recognition.</exception>
    public AudioSession(IModel model) : base(model)
    {
        if (model.Info.Task != "automatic-speech-recognition")
        {
            throw new ArgumentException(
                $"AudioSession requires a model with task 'automatic-speech-recognition', but got '{model.Info.Task}'.",
                nameof(model));
        }
    }
}
