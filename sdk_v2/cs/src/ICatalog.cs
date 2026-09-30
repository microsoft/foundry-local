// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;
using System.Collections.Generic;

public interface ICatalog
{
    /// <summary>
    /// The catalog name.
    /// </summary>
    string Name { get; }

    /// <summary>
    /// List the available models in the catalog.
    /// </summary>
    /// <param name="ct">Optional CancellationToken.</param>
    /// <returns>List of IModel instances.</returns>
    Task<List<IModel>> ListModelsAsync(CancellationToken? ct = null);

    /// <summary>
    /// Lookup a model by its alias.
    /// </summary>
    /// <param name="modelAlias">Model alias.</param>
    /// <param name="ct">Optional CancellationToken.</param>
    /// <returns>The matching IModel, or null if no model with the given alias exists.</returns>
    Task<IModel?> GetModelAsync(string modelAlias, CancellationToken? ct = null);

    /// <summary>
    /// Lookup a model variant by its unique model id.
    /// NOTE: This will return an IModel with a single variant. Use GetModelAsync to get an IModel with all available
    ///       variants.
    /// </summary>
    /// <param name="modelId">Model id.</param>
    /// <param name="ct">Optional CancellationToken.</param>
    /// <returns>The matching IModel, or null if no variant with the given id exists.</returns>
    Task<IModel?> GetModelVariantAsync(string modelId, CancellationToken? ct = null);

    /// <summary>
    /// Get a list of currently downloaded models from the model cache.
    /// </summary>
    /// <param name="ct">Optional CancellationToken.</param>
    /// <returns>One IModel instance per cached model variant.</returns>
    Task<List<IModel>> GetCachedModelsAsync(CancellationToken? ct = null);

    /// <summary>
    /// Get a list of the currently loaded models.
    /// </summary>
    /// <param name="ct">Optional CancellationToken.</param>
    /// <returns>List of IModel instances.</returns>
    Task<List<IModel>> GetLoadedModelsAsync(CancellationToken? ct = null);

    /// <summary>
    /// Get the set of available versions for a model alias, optionally filtered by variant name.
    /// </summary>
    /// <param name="modelAlias">Model alias.</param>
    /// <param name="modelName">Optional exact variant name filter; null matches all variants.</param>
    /// <param name="maxVersions">Maximum number of versions to return per variant name. Use 0 or a negative value to return all versions.</param>
    /// <param name="ct">Optional CancellationToken.</param>
    /// <returns>Available model versions matching the alias and variant filter.</returns>
    Task<List<IModel>> GetModelVersionsAsync(string modelAlias, string? modelName = null, int maxVersions = 50, CancellationToken? ct = null);

    /// <summary>
    /// Get the latest version of a model.
    /// This is used to check if a newer version of a model is available in the catalog for download.
    /// </summary>
    /// <param name="model">The model to check for the latest version.</param>
    /// <param name="ct">Optional CancellationToken.</param>
    /// <returns>The latest version of the model. Will match the input if it is the latest version.</returns>
    Task<IModel> GetLatestVersionAsync(IModel model, CancellationToken? ct = null);

    /// <summary>
    /// Register existing model assets in the local catalog without taking ownership of the directory. Task is
    /// required. Display name, publisher, validated runtime metadata, modalities, and custom properties are preserved
    /// from the synchronous <paramref name="metadata"/> snapshot and may receive authoritative defaults. Identity,
    /// alias, type, timestamps, context length, and prompt templates are SDK-derived; caller location and internal
    /// metadata are ignored. A caller-supplied provider becomes the default load override; an SDK-derived artifact
    /// provider is descriptive and leaves genai_config.json provider options intact.
    /// </summary>
    /// <param name="modelPath">Model directory containing a valid genai_config.json file.</param>
    /// <param name="modelId">Canonical model identifier in &lt;name&gt;:&lt;version&gt; format.</param>
    /// <param name="metadata">Model metadata. At minimum, set a supported task such as chat-completion.</param>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>A manager-owned model handle that remains valid for the manager's lifetime, including after unregister.</returns>
    /// <exception cref="FoundryLocalException">The catalog is not local or registration validation fails.</exception>
    Task<IModel> RegisterModelAsync(string modelPath, string modelId, ModelInfoBuilder metadata,
                                    CancellationToken? ct = null);

    /// <summary>
    /// Unregister a local model without deleting its assets. A model ID removes one variant; an alias removes every
    /// registered version and variant in that alias group.
    /// </summary>
    /// <param name="aliasOrModelId">A registered model alias or canonical model ID.</param>
    /// <param name="ct">Optional cancellation token.</param>
    /// <exception cref="FoundryLocalException">The catalog is not local or the registration cannot be removed.</exception>
    Task UnregisterModelAsync(string aliasOrModelId, CancellationToken? ct = null);
}
