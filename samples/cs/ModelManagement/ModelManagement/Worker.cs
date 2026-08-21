using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using ModelManagement.Interfaces;

namespace ModelManagement;

internal class Worker : BackgroundService
{
    private readonly ILogger<Worker> _logger;
    private readonly ICatalogManagement _catalogManagement;
    private readonly IEPManagement _epManagement;

    public Worker(IEPManagement epManagement, ICatalogManagement catalogManagement, ILogger<Worker> logger)
    {
        _epManagement = epManagement;
        _catalogManagement = catalogManagement;
        _logger = logger;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        _logger.LogInformation("Worker running at: {time}", DateTimeOffset.Now);

        await _epManagement.DownloadAndRegisterEpsAsync(stoppingToken);
        await _catalogManagement.DownloadModelsAsync(new List<string> { "qwen3-embedding-0.6b", "phi-3.5-mini" }, stoppingToken);
        List<(string, IModel)> result = await _catalogManagement.LoadModelsAsync(new List<string> { "qwen3-embedding-0.6b", "phi-3.5-mini" }, stoppingToken);

        while (!stoppingToken.IsCancellationRequested)
        {
            _logger.LogInformation("Heartbeat at: {time}", DateTimeOffset.Now);
            await Task.Delay(TimeSpan.FromSeconds(5), stoppingToken);
        }

        _logger.LogInformation("Worker stopping");
    }
}
