// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging;

internal sealed class Catalog : ICatalog, IDisposable
{
    private readonly Dictionary<string, Model> _modelAliasToModel = new();
    private readonly Dictionary<string, ModelVariant> _modelIdToModelVariant = new();
    private DateTime _lastFetch;

    private readonly IModelLoadManager _modelLoadManager;
    private readonly ICoreInterop _coreInterop;
    private readonly ILogger _logger;
    private readonly AsyncLock _lock = new();

    public string Name { get; init; }

    private Catalog(IModelLoadManager modelLoadManager, ICoreInterop coreInterop, ILogger logger)
    {
        _modelLoadManager = modelLoadManager;
        _coreInterop = coreInterop;
        _logger = logger;

        _lastFetch = DateTime.MinValue;

        CoreInteropRequest? input = null;
        var response = coreInterop.ExecuteCommand("get_catalog_name", input);
        if (response.Error != null)
        {
            throw new FoundryLocalException($"Error getting catalog name: {response.Error}", _logger);
        }

        Name = response.Data!;
    }

    internal static async Task<Catalog> CreateAsync(IModelLoadManager modelManager, ICoreInterop coreInterop,
                                                    ILogger logger, CancellationToken? ct = null)
    {
        var catalog = new Catalog(modelManager, coreInterop, logger);
        await catalog.UpdateModels(ct).ConfigureAwait(false);
        return catalog;
    }

    public async Task<List<IModel>> ListModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => ListModelsImplAsync(ct),
                                                     "Error listing models.", _logger).ConfigureAwait(false);
    }

    public async Task<List<IModel>> GetCachedModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => GetCachedModelsImplAsync(ct),
                                                     "Error getting cached models.", _logger).ConfigureAwait(false);
    }

    public async Task<List<IModel>> GetLoadedModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => GetLoadedModelsImplAsync(ct),
                                                     "Error getting loaded models.", _logger).ConfigureAwait(false);
    }

    public async Task<IModel?> GetModelAsync(string modelAlias, CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => GetModelImplAsync(modelAlias, ct),
                                                     $"Error getting model with alias '{modelAlias}'.", _logger)
                                                    .ConfigureAwait(false);
    }

    public async Task<IModel?> GetModelVariantAsync(string modelId, CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => GetModelVariantImplAsync(modelId, ct),
                                                     $"Error getting model variant with ID '{modelId}'.", _logger)
                                                    .ConfigureAwait(false);
    }

    public async Task<IModel> GetLatestVersionAsync(IModel modelOrModelVariant, CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(
            () => GetLatestVersionImplAsync(modelOrModelVariant, ct),
            $"Error getting latest version for model with name '{modelOrModelVariant.Info.Name}'.",
            _logger).ConfigureAwait(false);
    }

    private async Task<List<IModel>> ListModelsImplAsync(CancellationToken? ct = null)
    {
        await UpdateModels(ct).ConfigureAwait(false);

        using var disposable = await _lock.LockAsync().ConfigureAwait(false);
        return _modelAliasToModel.Values.OrderBy(m => m.Alias).Cast<IModel>().ToList();
    }

    private async Task<List<IModel>> GetCachedModelsImplAsync(CancellationToken? ct = null)
    {
        await UpdateModels(ct).ConfigureAwait(false);

        var cachedModelIds = await Utils.GetCachedModelIdsAsync(_coreInterop, ct).ConfigureAwait(false);
        return await ResolveModelIdsAsync(cachedModelIds, ct).ConfigureAwait(false);
    }

    private async Task<List<IModel>> GetLoadedModelsImplAsync(CancellationToken? ct = null)
    {
        await UpdateModels(ct).ConfigureAwait(false);

        var loadedModelIds = await _modelLoadManager.ListLoadedModelsAsync(ct).ConfigureAwait(false);
        return await ResolveModelIdsAsync(loadedModelIds, ct).ConfigureAwait(false);
    }

    // Resolve a list of model ids against the in-memory catalog, self-healing once
    // if any id is unknown (e.g. a manually-added BYOM model the SDK has not yet seen).
    // Preserves the input order of modelIds (minus unknowns).
    private async Task<List<IModel>> ResolveModelIdsAsync(string[] modelIds, CancellationToken? ct)
    {
        bool needsRefresh = false;
        using (var disposable = await _lock.LockAsync().ConfigureAwait(false))
        {
            foreach (var modelId in modelIds)
            {
                if (!_modelIdToModelVariant.ContainsKey(modelId))
                {
                    needsRefresh = true;
                    break;
                }
            }
        }

        if (needsRefresh)
        {
            await UpdateModels(ct, force: true).ConfigureAwait(false);
        }

        List<IModel> resolved = new(modelIds.Length);
        using (var disposable = await _lock.LockAsync().ConfigureAwait(false))
        {
            foreach (var modelId in modelIds)
            {
                if (_modelIdToModelVariant.TryGetValue(modelId, out ModelVariant? variant))
                {
                    resolved.Add(variant);
                }
            }
        }

        return resolved;
    }

    private async Task<Model?> GetModelImplAsync(string modelAlias, CancellationToken? ct = null)
    {
        if (string.IsNullOrWhiteSpace(modelAlias))
        {
            return null;
        }

        await UpdateModels(ct).ConfigureAwait(false);

        Model? model;
        using (var disposable = await _lock.LockAsync().ConfigureAwait(false))
        {
            _modelAliasToModel.TryGetValue(modelAlias, out model);
        }
        if (model is not null)
        {
            return model;
        }

        // Self-heal: the alias may belong to a BYOM model added to the cache
        // directory after our last catalog refresh.
        await UpdateModels(ct, force: true).ConfigureAwait(false);
        using (var disposable = await _lock.LockAsync().ConfigureAwait(false))
        {
            _modelAliasToModel.TryGetValue(modelAlias, out model);
        }
        return model;
    }

    private async Task<ModelVariant?> GetModelVariantImplAsync(string modelId, CancellationToken? ct = null)
    {
        if (string.IsNullOrWhiteSpace(modelId))
        {
            return null;
        }

        await UpdateModels(ct).ConfigureAwait(false);

        ModelVariant? modelVariant;
        using (var disposable = await _lock.LockAsync().ConfigureAwait(false))
        {
            _modelIdToModelVariant.TryGetValue(modelId, out modelVariant);
        }
        if (modelVariant is not null)
        {
            return modelVariant;
        }

        // Self-heal: the id may belong to a BYOM model added to the cache
        // directory after our last catalog refresh.
        await UpdateModels(ct, force: true).ConfigureAwait(false);
        using (var disposable = await _lock.LockAsync().ConfigureAwait(false))
        {
            _modelIdToModelVariant.TryGetValue(modelId, out modelVariant);
        }
        return modelVariant;
    }

    private async Task<IModel> GetLatestVersionImplAsync(IModel modelOrModelVariant, CancellationToken? ct)
    {
        Model? model;

        if (modelOrModelVariant is ModelVariant)
        {
            // For ModelVariant, resolve the owning Model via alias.
            model = await GetModelImplAsync(modelOrModelVariant.Alias, ct);
        }
        else
        {
            // Try to use the concrete Model instance if this is our SDK type.
            model = modelOrModelVariant as Model;

            // If this is a different IModel implementation (e.g., a test stub),
            // fall back to resolving the Model via alias.
            if (model == null)
            {
                model = await GetModelImplAsync(modelOrModelVariant.Alias, ct);
            }
        }

        if (model == null)
        {
            throw new FoundryLocalException($"Model with alias '{modelOrModelVariant.Alias}' not found in catalog.",
                                            _logger);
        }

        // variants are sorted by version, so the first one matching the name is the latest version for that variant.
        var latest = model!.Variants.FirstOrDefault(v => v.Info.Name == modelOrModelVariant.Info.Name) ??
            // should not be possible given we internally manage all the state involved
            throw new FoundryLocalException($"Internal error. Mismatch between model (alias:{model.Alias}) and " +
                                            $"model variant (alias:{modelOrModelVariant.Alias}).", _logger);

        // if input was the latest return the input (could be model or model variant)
        // otherwise return the latest model variant
        return latest.Id == modelOrModelVariant.Id ? modelOrModelVariant : latest;
    }

    private async Task UpdateModels(CancellationToken? ct, bool force = false)
    {
        // TODO: make this configurable
        // Skip if the cache is still fresh, unless the caller forced a refresh
        // (e.g. self-heal after a cache miss caused by a manually-added BYOM
        // model dropped into the cache directory).
        if (!force && DateTime.Now - _lastFetch < TimeSpan.FromHours(6))
        {
            return;
        }

        CoreInteropRequest? input = null;
        var result = await _coreInterop.ExecuteCommandAsync("get_model_list", input, ct).ConfigureAwait(false);

        if (result.Error != null)
        {
            throw new FoundryLocalException($"Error getting models: {result.Error}", _logger);
        }

        var models = JsonSerializer.Deserialize(result.Data!, JsonSerializationContext.Default.ListModelInfo);
        if (models == null)
        {
            _logger.LogDebug($"ListModelInfo deserialization error in UpdateModels. Data: {result.Data}");
            throw new FoundryLocalException($"Failed to deserialize models from response.", _logger);
        }

        using var disposable = await _lock.LockAsync().ConfigureAwait(false);

        // Incremental refresh: preserve wrapper identity for ids/aliases that
        // survive the refresh so externally-held IModel references keep
        // working with up-to-date metadata and (for Model) keep any explicit
        // SelectVariant() choice. New ids get fresh wrappers; removed ids get
        // evicted.

        var freshIds = new HashSet<string>(StringComparer.Ordinal);
        var freshAliasGroups = new Dictionary<string, List<ModelInfo>>(StringComparer.Ordinal);
        foreach (var info in models)
        {
            freshIds.Add(info.Id);
            if (!freshAliasGroups.TryGetValue(info.Alias, out var group))
            {
                group = [];
                freshAliasGroups[info.Alias] = group;
            }
            group.Add(info);
        }

        foreach (var staleId in _modelIdToModelVariant.Keys.Where(id => !freshIds.Contains(id)).ToList())
        {
            _modelIdToModelVariant.Remove(staleId);
        }
        foreach (var staleAlias in _modelAliasToModel.Keys.Where(a => !freshAliasGroups.ContainsKey(a)).ToList())
        {
            _modelAliasToModel.Remove(staleAlias);
        }

        foreach (var info in models)
        {
            if (_modelIdToModelVariant.TryGetValue(info.Id, out var existing))
            {
                existing.RefreshInfo(info);
            }
            else
            {
                _modelIdToModelVariant[info.Id] = new ModelVariant(info, _modelLoadManager, _coreInterop, _logger);
            }
        }

        foreach (var kvp in freshAliasGroups)
        {
            var alias = kvp.Key;
            var aliasInfos = kvp.Value;
            var aliasVariants = aliasInfos.ConvertAll(i => _modelIdToModelVariant[i.Id]);
            if (_modelAliasToModel.TryGetValue(alias, out var existingModel))
            {
                existingModel.RefreshVariants(aliasVariants);
            }
            else
            {
                var m = new Model(aliasVariants[0], _logger);
                for (var i = 1; i < aliasVariants.Count; i++)
                {
                    m.AddVariant(aliasVariants[i]);
                }
                _modelAliasToModel[alias] = m;
            }
        }

        _lastFetch = DateTime.Now;
    }

    internal void InvalidateCache()
    {
        _lastFetch = DateTime.MinValue;
    }

    public void Dispose()
    {
        _lock.Dispose();
    }
}
