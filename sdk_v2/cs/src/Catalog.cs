// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Collections.Generic;
using System.Threading.Tasks;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging;

using NativeCatalog = Microsoft.AI.Foundry.Local.Detail.Native.Catalog;
using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;
using NativeModelInfo = Microsoft.AI.Foundry.Local.Detail.Native.MutableModelInfo;

internal sealed class Catalog : ICatalog
{
    private readonly NativeCatalog _nativeCatalog;
    private readonly ILogger _logger;
    private readonly ManagerLifetime _nativeLifetime;

    public string Name { get; }

    internal Catalog(NativeCatalog nativeCatalog, ILogger logger, ManagerLifetime nativeLifetime)
    {
        _nativeCatalog = nativeCatalog;
        _logger = logger;
        _nativeLifetime = nativeLifetime;
        Name = _nativeCatalog.GetName();
    }

    public async Task<List<IModel>> ListModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(() =>
            {
                using var list = _nativeCatalog.GetModels();
                return list.Models.Select(CreateModel).ToList();
            }),
            "Error listing models.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<List<IModel>> GetCachedModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(() =>
            {
                using var list = _nativeCatalog.GetCachedModels();
                return list.Models.Select(CreateModel).ToList();
            }),
            "Error getting cached models.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<List<IModel>> GetLoadedModelsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(() =>
            {
                using var list = _nativeCatalog.GetLoadedModels();
                return list.Models.Select(CreateModel).ToList();
            }),
            "Error getting loaded models.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<List<IModel>> GetModelVersionsAsync(string modelAlias, string? modelName = null, int maxVersions = 50, CancellationToken? ct = null)
    {
        Detail.Throw.IfContainsEmbeddedNul(modelAlias);
        if (modelName != null)
        {
            Detail.Throw.IfContainsEmbeddedNul(modelName);
        }
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(() =>
            {
                using var list = _nativeCatalog.GetModelVersions(modelAlias, modelName, maxVersions);
                return list.Models.Select(CreateModel).ToList();
            }),
            $"Error getting model versions for alias '{modelAlias}'.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<IModel?> GetModelAsync(string modelAlias, CancellationToken? ct = null)
    {
        Detail.Throw.IfContainsEmbeddedNul(modelAlias);
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(() =>
            {
                var model = _nativeCatalog.GetModel(modelAlias);
                return model != null ? CreateModel(model) : null;
            }),
            $"Error getting model with alias '{modelAlias}'.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<IModel?> GetModelVariantAsync(string modelId, CancellationToken? ct = null)
    {
        Detail.Throw.IfContainsEmbeddedNul(modelId);
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(() =>
            {
                var model = _nativeCatalog.GetModelVariant(modelId);
                return model != null ? CreateModel(model) : null;
            }),
            $"Error getting model variant with ID '{modelId}'.", _logger, ct).ConfigureAwait(false);
    }

    public async Task<IModel> GetLatestVersionAsync(IModel model, CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(() =>
            {
                var inputModel = (Model)model;
                if (!ReferenceEquals(_nativeLifetime, inputModel.NativeLifetime))
                {
                    throw new ArgumentException("Model must belong to this catalog's manager.", nameof(model));
                }
                var latest = _nativeCatalog.GetLatestVersion(inputModel.NativeModel);
                return CreateModel(latest);
            }),
            $"Error getting latest version for model with name '{model.Info.Name}'.",
            _logger, ct).ConfigureAwait(false);
    }

    public async Task<IModel> RegisterModelAsync(string modelPath, string modelId, ModelInfoBuilder metadata,
                                                 CancellationToken? ct = null)
    {
        Detail.Throw.IfNull(metadata);
        Detail.Throw.IfContainsEmbeddedNul(modelPath);
        Detail.Throw.IfContainsEmbeddedNul(modelId);

        using var nativeMetadata = new NativeModelInfo();
        PopulateNativeMetadata(nativeMetadata, metadata);

        return await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(
                () => CreateModel(_nativeCatalog.RegisterModel(modelPath, modelId, nativeMetadata))),
            $"Error registering model '{modelId}'.", _logger, ct).ConfigureAwait(false);
    }

    public async Task UnregisterModelAsync(string aliasOrModelId, CancellationToken? ct = null)
    {
        Detail.Throw.IfContainsEmbeddedNul(aliasOrModelId);
        await Utils.CallWithExceptionHandlingAsync(
            () => WithNativeCatalog(() => _nativeCatalog.UnregisterModel(aliasOrModelId)),
            $"Error unregistering model '{aliasOrModelId}'.", _logger, ct).ConfigureAwait(false);
    }

    private ManagerLifetime.Lease AcquireManagerLease() =>
        _nativeLifetime.Acquire(this, trackReentrancy: true);

    private TResult WithNativeCatalog<TResult>(Func<TResult> operation)
    {
        using (AcquireManagerLease())
        {
            return operation();
        }
    }

    private void WithNativeCatalog(Action operation)
    {
        using (AcquireManagerLease())
        {
            operation();
        }
    }

    private IModel CreateModel(NativeModel model) =>
        new Model(model, _logger, _nativeLifetime);

    private static void PopulateNativeMetadata(NativeModelInfo nativeMetadata, ModelInfoBuilder metadata)
    {
        foreach (var property in metadata.StringProperties)
        {
            nativeMetadata.SetStringProperty(property.Key, property.Value);
        }

        foreach (var property in metadata.IntProperties)
        {
            nativeMetadata.SetIntProperty(property.Key, property.Value);
        }
    }

}
