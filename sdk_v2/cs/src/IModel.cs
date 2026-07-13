// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Threading;
using System.Threading.Tasks;

public interface IModel
{
    string Id { get; }
    [System.Diagnostics.CodeAnalysis.SuppressMessage("Naming", "CA1716:Identifiers should not match keywords",
        Justification = "Alias is a suitable name in this context.")]
    string Alias { get; }

    ModelInfo Info { get; }

    Task<bool> IsCachedAsync(CancellationToken? ct = null);
    Task<bool> IsLoadedAsync(CancellationToken? ct = null);

    /// <summary>
    /// Download the model to local cache if not already present.
    /// </summary>
    /// <param name="downloadProgress">
    /// Optional progress callback for download progress.
    /// Percentage download (0 - 100.0) is reported.</param>
    /// <param name="ct">Optional cancellation token.</param>
    Task DownloadAsync(Action<float>? downloadProgress = null,
                       CancellationToken? ct = null);

    /// <summary>
    /// Gets the model path if cached.
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>Path of model directory.</returns>
    Task<string> GetPathAsync(CancellationToken? ct = null);

    /// <summary>
    /// Load the model into memory if not already loaded.
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    Task LoadAsync(CancellationToken? ct = null);

    /// <summary>
    /// Remove the model from the local cache.
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    Task RemoveFromCacheAsync(CancellationToken? ct = null);

    /// <summary>
    /// Unload the model if loaded.
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    Task UnloadAsync(CancellationToken? ct = null);

    /// <summary>
    /// Get an OpenAI API based ChatClient
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>OpenAI.ChatClient</returns>
    [System.Obsolete("Use new ChatSession(model) instead.", error: false)]
    Task<OpenAIChatClient> GetChatClientAsync(CancellationToken? ct = null);

    /// <summary>
    /// Get an OpenAI API based AudioClient
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>OpenAI.AudioClient</returns>
    [System.Obsolete("Use new AudioSession(model) instead.", error: false)]
    Task<OpenAIAudioClient> GetAudioClientAsync(CancellationToken? ct = null);

    /// <summary>
    /// Get an OpenAI API based EmbeddingClient.
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>OpenAI.EmbeddingClient</returns>
    [System.Obsolete("Use new EmbeddingsSession(model) instead.", error: false)]
    Task<OpenAIEmbeddingClient> GetEmbeddingClientAsync(CancellationToken? ct = null);

    /// <summary>
    /// Variants of the model that are available. Variants of the model are optimized for different devices.
    /// </summary>
    IReadOnlyList<IModel> Variants { get; }

    /// <summary>
    /// Select a model variant from <see cref="Variants"/> to use for <see cref="IModel"/> operations.
    /// An IModel from `Variants` can also be used directly.
    /// </summary>
    /// <param name="variant">Model variant to select. Must be one of the variants in <see cref="Variants"/>.</param>
    /// <exception cref="FoundryLocalException">If variant is not valid for this model.</exception>
    void SelectVariant(IModel variant);
}
