using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Microsoft.Extensions.Options;
using ModelManagement.Interfaces;
using Serilog;
using System;
using System.Threading;
using System.Threading.Tasks;

namespace ModelManagement;

internal class Program
{
    private static async Task Main(string[] args)
    {
        var _loggerFactory = Microsoft.Extensions.Logging.LoggerFactory.Create(builder =>
        {
            builder.SetMinimumLevel(Microsoft.Extensions.Logging.LogLevel.Information);
        });
        var _logger = _loggerFactory.CreateLogger("ModelManagement");

        // Bootstrapping Serilog from appsettings.json explicitly so logging is active
        var configuration = new ConfigurationBuilder()
            .SetBasePath(AppContext.BaseDirectory)
            .AddJsonFile("appsettings.json", optional: true, reloadOnChange: true)
            .AddEnvironmentVariables()
            .Build();

        Log.Logger = new LoggerConfiguration()
            .ReadFrom.Configuration(configuration)
            .Enrich.FromLogContext()
            .CreateLogger();

        try
        {
            // Initialize the manager first (see Quick Start)
            await FoundryLocalManager.CreateAsync(
                new Configuration { AppName = "my-app" },
                _logger);
            var mgr = FoundryLocalManager.Instance;

            Log.Information("Starting host");

            var builder = Host.CreateDefaultBuilder(args)
                .ConfigureAppConfiguration((hostingContext, config) =>
                {
                    // allow host to read configuration too
                    config.AddConfiguration(configuration);
                })
                .UseSerilog() // use the static Log.Logger we created
                .ConfigureServices((context, services) =>
                {
                    // Register the FoundryLocalManager singleton instance created earlier
                    services.AddSingleton(FoundryLocalManager.Instance);

                    // Register EPManagement for IEPManagement so it can be injected
                    services.AddSingleton<IEPManagement, EPManagement>();
                    services.AddScoped<ICatalogManagement, CatalogManagement>();

                    services.AddHostedService<Worker>();
                });

            using var host = builder.Build();

            await host.RunAsync();
        }
        catch (Exception ex)
        {
            Log.Fatal(ex, "Host terminated unexpectedly");
            throw;
        }
        finally
        {
            Log.CloseAndFlush();
        }
    }
}