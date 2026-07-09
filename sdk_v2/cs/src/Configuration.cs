// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

public class Configuration
{
    /// <summary>
    /// Your application name. MUST be set to a valid name.
    /// </summary>
    public required string AppName { get; set; }

    /// <summary>
    /// Application data directory.
    /// Default: {home}/.{appname}, where {home} is the user's home directory and {appname} is the AppName value.
    /// </summary>
    public string? AppDataDir { get; init; }

    /// <summary>
    /// Model cache directory.
    /// Default: {appdata}/cache/models, where {appdata} is the AppDataDir value.
    /// </summary>
    public string? ModelCacheDir { get; init; }

    /// <summary>
    /// Log directory.
    /// Default: {appdata}/logs
    /// </summary>
    public string? LogsDir { get; init; }

    /// <summary>
    /// Logging level.
    /// Valid values are: Verbose, Debug, Information, Warning, Error, Fatal.
    /// Default: LogLevel.Warning
    /// </summary>
    public LogLevel LogLevel { get; init; } = LogLevel.Warning;

    /// <summary>
    /// Optional configuration for the built-in web service.
    /// NOTE: This is not included in all builds.
    /// </summary>
    public WebService? Web { get; init; }

    /// <summary>
    /// Additional settings that Foundry Local Core can consume.
    /// Keys and values are strings.
    /// </summary>
    public IDictionary<string, string>? AdditionalSettings { get; init; }

    /// <summary>
    /// Optional. Azure region for the model registry download endpoint
    /// (https://{region}.api.azureml.ms/modelregistry/...).
    /// Defaults to "centralus" when not set.
    /// </summary>
    public string? CatalogRegion { get; init; }

    /// <summary>
    /// Catalog URLs with optional per-catalog filter overrides.
    /// Each entry is a (url, filter) pair where filter may be null to use the default.
    /// Defaults to the Azure Foundry Local Catalog if empty.
    /// </summary>
    public IList<(string Url, string? Filter)>? CatalogUrls { get; init; }

    /// <summary>
    /// Configuration settings if the optional web service is used.
    /// </summary>
    public class WebService
    {
        /// <summary>
        /// Url/s to bind to the web service when <see cref="FoundryLocalManager.StartWebServiceAsync"/> is called.
        /// After startup, <see cref="FoundryLocalManager.Urls"/> will contain the actual URL/s the service is listening on.
        /// 
        /// Default: 127.0.0.1:0, which binds to a random ephemeral port.
        /// Multiple URLs can be specified as a semi-colon separated list.
        /// </summary>
        public string? Urls { get; init; }

        /// <summary>
        /// Optional. URL of an external Foundry Local service (typically the long-running
        /// service process). When set, the catalog operates in cache-only mode — it reads
        /// only the local disk cache populated by that external service and skips network
        /// and local-model scans. Reserved for future delegation of model load/unload
        /// to the external service.
        /// </summary>
        /// <remarks>
        /// Both processes should be using the same version of the SDK. If a random port is
        /// assigned when creating the web service in the external process, the actual port
        /// must be provided here.
        /// </remarks>
        public Uri? ExternalUrl { get; init; }
    }

    internal void Validate()
    {
        if (string.IsNullOrEmpty(AppName))
        {
            throw new ArgumentException("Configuration AppName must be set to a valid application name.");
        }

        if (AppName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
        {
            throw new ArgumentException("Configuration AppName value contains invalid characters.");
        }

        if (Web?.ExternalUrl?.Port == 0)
        {
            throw new ArgumentException("Configuration Web.ExternalUrl has invalid port of 0.");
        }
    }
}
