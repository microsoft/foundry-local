using Microsoft.AI.Foundry.Local;

namespace ModelManagement.Interfaces
{
    public interface ICatalogManagement
    {
        Task DownloadModelsAsync(List<string> modelNames, CancellationToken ct = default);
        Task<List<(string, IModel)>> LoadModelsAsync(List<string> modelNames, CancellationToken ct = default);
    }
}