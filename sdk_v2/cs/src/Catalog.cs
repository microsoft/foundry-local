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
using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;
using NativeModelInfo = Microsoft.AI.Foundry.Local.Detail.Native.MutableModelInfo;

internal sealed class Catalog : ICatalog
{
    private readonly NativeCatalog _nativeCatalog;
    private readonly ILogger _logger;
    private readonly object _nativeLifetimeLock;
    private readonly Func<bool> _isManagerDisposed;

    public string Name { get; }

    internal Catalog(NativeCatalog nativeCatalog, ILogger logger, object nativeLifetimeLock,
                     Func<bool> isManagerDisposed)
    {
        _nativeCatalog = nativeCatalog;
        _logger = logger;
        _nativeLifetimeLock = nativeLifetimeLock;
        _isManagerDisposed = isManagerDisposed;
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

    public async Task<List<IModel>> GetModelVersionsAsync(string modelAlias, string? modelName = null, int maxVersions = 50, CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                using var list = _nativeCatalog.GetModelVersions(modelAlias, modelName, maxVersions);
                return list.Models.Select(m => (IModel)new Model(m, _logger)).ToList();
            },
            $"Error getting model versions for alias '{modelAlias}'.", _logger, ct).ConfigureAwait(false);
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

    public async Task<IModel> RegisterModelAsync(string modelPath, string modelId, ModelInfo metadata,
                                                 CancellationToken? ct = null)
    {
        Detail.Throw.IfNull(metadata);

        using var nativeMetadata = new NativeModelInfo();
        PopulateNativeMetadata(nativeMetadata, metadata);

        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                NativeModel model;
                lock (_nativeLifetimeLock)
                {
                    ThrowIfManagerDisposed();
                    model = _nativeCatalog.RegisterModel(modelPath, modelId, nativeMetadata);
                }
                return (IModel)new Model(model, _logger);
            },
            $"Error registering model '{modelId}'.", _logger, ct).ConfigureAwait(false);
    }

    public async Task UnregisterModelAsync(string aliasOrModelId, CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                lock (_nativeLifetimeLock)
                {
                    ThrowIfManagerDisposed();
                    _nativeCatalog.UnregisterModel(aliasOrModelId);
                }
            },
            $"Error unregistering model '{aliasOrModelId}'.", _logger, ct).ConfigureAwait(false);
    }

    private void ThrowIfManagerDisposed()
    {
        Detail.Throw.IfDisposed(_isManagerDisposed(), this);
    }

    private static void PopulateNativeMetadata(NativeModelInfo nativeMetadata, ModelInfo metadata)
    {
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.DisplayName, metadata.DisplayName);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.ModelType, metadata.ModelType);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.Publisher, metadata.Publisher);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.License, metadata.License);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.LicenseDescription, metadata.LicenseDescription);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.Task, metadata.Task);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.ModelProvider, metadata.ProviderType);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.MinimumFoundryLocalVersion, metadata.MinFLVersion);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.InputModalities, metadata.InputModalities);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.OutputModalities, metadata.OutputModalities);
        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.Capabilities, metadata.Capabilities);

        if (metadata.Runtime?.DeviceType is DeviceType.CPU or DeviceType.GPU or DeviceType.NPU)
        {
            nativeMetadata.SetStringProperty(ModelInfoPropertyKeys.DeviceType, metadata.Runtime.DeviceType.ToString());
        }

        SetStringIfNotEmpty(nativeMetadata, ModelInfoPropertyKeys.ExecutionProvider,
                            metadata.Runtime?.ExecutionProvider);

        if (metadata.FileSizeMb.HasValue)
        {
            nativeMetadata.SetIntProperty(ModelInfoPropertyKeys.FileSizeMb, metadata.FileSizeMb.Value);
        }

        if (metadata.SupportsToolCalling.HasValue)
        {
            nativeMetadata.SetIntProperty(ModelInfoPropertyKeys.SupportsToolCalling,
                                          metadata.SupportsToolCalling.Value ? 1 : 0);
        }

        if (metadata.MaxOutputTokens.HasValue)
        {
            nativeMetadata.SetIntProperty(ModelInfoPropertyKeys.MaxOutputTokens, metadata.MaxOutputTokens.Value);
        }

        if (metadata.CreatedAtUnix != 0)
        {
            nativeMetadata.SetIntProperty(ModelInfoPropertyKeys.CreatedAtUnix, metadata.CreatedAtUnix);
        }

        if (metadata.ContextLength.HasValue)
        {
            nativeMetadata.SetIntProperty(ModelInfoPropertyKeys.ContextLength, metadata.ContextLength.Value);
        }

        foreach (var property in metadata.StringProperties)
        {
            nativeMetadata.SetStringProperty(property.Key, property.Value);
        }

        foreach (var property in metadata.IntProperties)
        {
            nativeMetadata.SetIntProperty(property.Key, property.Value);
        }
    }

    private static void SetStringIfNotEmpty(NativeModelInfo metadata, string key, string? value)
    {
        if (!string.IsNullOrEmpty(value))
        {
            metadata.SetStringProperty(key, value!);
        }
    }
}
