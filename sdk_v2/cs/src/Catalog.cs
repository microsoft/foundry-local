// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Collections.Generic;
using System.Threading.Tasks;

using Microsoft.Extensions.Logging;

using NativeCatalog = Microsoft.AI.Foundry.Local.Detail.Native.Catalog;

internal sealed class Catalog : ICatalog
{
    private readonly NativeCatalog _nativeCatalog;
    private readonly ILogger _logger;

    public string Name { get; }

    internal Catalog(NativeCatalog nativeCatalog, ILogger logger)
    {
        _nativeCatalog = nativeCatalog;
        _logger = logger;
        Name = _nativeCatalog.GetName();
    }

    public async Task<List<IModel>> ListModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                using var list = _nativeCatalog.GetModels();
                return list.Models.Select(m => (IModel)new Model(m, _logger)).ToList();
            },
            "Error listing models.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<List<IModel>> GetCachedModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                using var list = _nativeCatalog.GetCachedModels();
                return list.Models.Select(m => (IModel)new Model(m, _logger)).ToList();
            },
            "Error getting cached models.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<List<IModel>> GetLoadedModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                using var list = _nativeCatalog.GetLoadedModels();
                return list.Models.Select(m => (IModel)new Model(m, _logger)).ToList();
            },
            "Error getting loaded models.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<IModel?> GetModelAsync(string modelAlias, CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                var m = _nativeCatalog.GetModel(modelAlias);
                return m != null ? (IModel?)new Model(m, _logger) : null;
            },
            $"Error getting model with alias '{modelAlias}'.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<IModel?> GetModelVariantAsync(string modelId, CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                var m = _nativeCatalog.GetModelVariant(modelId);
                return m != null ? (IModel?)new Model(m, _logger) : null;
            },
            $"Error getting model variant with ID '{modelId}'.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<IModel> GetLatestVersionAsync(IModel model, CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                var inputModel = (Model)model;
                var latest = _nativeCatalog.GetLatestVersion(inputModel.NativeModel);
                return (IModel)new Model(latest, _logger);
            },
            $"Error getting latest version for model with name '{model.Info.Name}'.",
            _logger, ct).ConfigureAwait(false);
    }
}
