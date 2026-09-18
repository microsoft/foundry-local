using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging;
using ModelManagement.Interfaces;
using System;
using System.Collections.Generic;
using System.Text;

namespace ModelManagement
{
    public class EPManagement : IEPManagement
    {
        private readonly FoundryLocalManager _mgr;
        private readonly ILogger<EPManagement> _logger;

        public EPManagement(FoundryLocalManager mgr, ILogger<EPManagement> logger)
        { 
            _mgr = mgr; 
            _logger = logger;
        }

        public async Task DownloadAndRegisterEpsAsync(CancellationToken ct = default)
        {
            // Discover what EPs are available
            var discoveredEps = _mgr.DiscoverEps();
            List<string> epNames = new List<string>();
            foreach (var ep in discoveredEps)
            {
                _logger.LogInformation($"{ep.Name} — registered: {ep.IsRegistered}");
                if (!ep.IsRegistered)
                {
                    epNames.Add(ep.Name);
                }
            }

            // Download and register all EPs
            string currentEp = "";
            var result = await _mgr.DownloadAndRegisterEpsAsync(epNames, (epName, percent) =>
            {
                if (epName != currentEp)
                {
                    if (currentEp != "")
                    {
                        _logger.LogInformation("");
                    }
                    currentEp = epName;
                }
                _logger.LogInformation($"\r  {epName}  {percent,6:F1}%");
            }, ct);
            _logger.LogInformation("");

            _logger.LogInformation($"Success: {result.Success}, Status: {result.Status}");
        }
    }
}
