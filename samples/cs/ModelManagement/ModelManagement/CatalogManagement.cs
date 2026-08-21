using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging;
using ModelManagement.Interfaces;
using System;
using System.Collections.Generic;
using System.Text;

namespace ModelManagement
{
    public class CatalogManagement : ICatalogManagement
    {
        private readonly FoundryLocalManager _mgr;
        private readonly ILogger<CatalogManagement> _logger;

        public CatalogManagement(FoundryLocalManager mgr, ILogger<CatalogManagement> logger)
        {
            _mgr = mgr;
            _logger = logger;
        }

        public async Task DownloadModelsAsync(List<string> modelNames, CancellationToken ct = default)
        {
            ICatalog catalog = await _mgr.GetCatalogAsync(ct);

            List<IModel> cachedModels = await catalog.GetCachedModelsAsync(ct);
            List<IModel> availableModels = await catalog.ListModelsAsync(ct);

            foreach (var modelName in modelNames)
            {
                var model = availableModels.Find(m => m.Alias == modelName);
                if (model != null)
                {
                    if (cachedModels.Find(m => m.Alias == modelName) != null)
                    {
                        _logger.LogInformation($"Model already cached: {model.Alias}");
                        continue;
                    }

                    _logger.LogInformation($"Downloading model: {model.Alias}");
                    await model.DownloadAsync();
                }
                else
                {
                    _logger.LogWarning($"Model not found in catalog: {modelName}");
                }
            }
        }

        public async Task<List<(string, IModel)>> LoadModelsAsync(List<string> modelNames, CancellationToken ct = default)
        {
            List<(string, IModel)> result = new List<(string, IModel)>();

            ICatalog catalog = await _mgr.GetCatalogAsync(ct);

            List<IModel> cachedModels = await catalog.GetCachedModelsAsync(ct);
            List<IModel> loadedModels = await catalog.GetLoadedModelsAsync(ct);
            List<IModel> availableModels = await catalog.ListModelsAsync(ct);

            foreach (var modelName in modelNames)
            {
                var model = availableModels.Find(m => m.Alias == modelName);
                if (model != null)
                {
                    if (loadedModels.Find(m => m.Alias == modelName) == null)
                    {
                        _logger.LogInformation($"Model not loaded: {model.Alias}");

                        if (cachedModels.Find(m => m.Alias == modelName) == null)
                        {
                            _logger.LogInformation($"Downloading model: {model.Alias}");
                            await model.DownloadAsync();
                        }
                        else
                        {
                            _logger.LogInformation($"Model already cached: {model.Alias}");
                        }

                        _logger.LogInformation($"Loading model: {model.Alias}");
                        await model.LoadAsync();
                    }
                    else
                    {
                        _logger.LogInformation($"Model already loaded: {model.Alias}");
                    }

                    result.Add((modelName, model));
                }
                else
                {
                    _logger.LogWarning($"Model not found in catalog: {modelName}");
                }

            }

            return result;
        }


        public async Task UnloadModels(List<IModel> models, CancellationToken ct = default)
        {
            foreach (var model in models)
            {
                _logger.LogInformation($"Unloading model: {model.Alias}");
                await model.UnloadAsync(ct);
            }
        }

        public async Task ClearCacheAsync(CancellationToken ct = default)
        {
            ICatalog catalog = await _mgr.GetCatalogAsync(ct);

            List<IModel> cachedModels = await catalog.GetCachedModelsAsync(ct);
            foreach(var model in cachedModels)
            {
                _logger.LogInformation($"Removing model from cache: {model.Alias}");
                await model.RemoveFromCacheAsync(ct);
            }
        }

        public async Task RemoveModelsFromCacheAsync(List<IModel> models, CancellationToken ct = default)
        {
            foreach (var model in models)
            {
                _logger.LogInformation($"Removing model from cache: {model.Alias}");
                await model.RemoveFromCacheAsync(ct);
            }
        }

    }
}
