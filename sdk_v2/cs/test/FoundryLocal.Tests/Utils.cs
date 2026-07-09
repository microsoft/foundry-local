// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging;

#pragma warning disable CS0618 // Test helpers exercise PromptTemplate/ModelSettings which are obsolete but still supported.

internal static class Utils
{
    /// <summary>
    /// Resolves a path under the test project's <c>testdata/</c> output directory.
    /// </summary>
    internal static string TestDataPath(string filename) =>
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "testdata", filename));

    internal struct TestCatalogInfo
    {
        internal readonly List<ModelInfo> TestCatalog { get; }
        internal readonly string ModelListJson { get; }

        internal TestCatalogInfo(bool includeCuda)
        {

            TestCatalog = Utils.BuildTestCatalog(includeCuda);
            ModelListJson = JsonSerializer.Serialize(TestCatalog, JsonSerializationContext.Default.ListModelInfo);
        }
    }

    internal static readonly TestCatalogInfo TestCatalog = new(true);

    /// <summary>
    /// True when the integration test infrastructure initialized successfully — i.e. the
    /// shared model cache directory exists AND <see cref="FoundryLocalManager.CreateAsync"/>
    /// completed without throwing. Tests that depend on a live manager must gate themselves
    /// with <see cref="SkipUnlessIntegrationAttribute"/> so they skip gracefully when this
    /// is false (build-output-only runs, sanitizer jobs, packaging-only CI, etc).
    /// </summary>
    internal static bool IntegrationTestsAvailable { get; private set; }

    static Utils()
    {
        using var loggerFactory = LoggerFactory.Create(builder =>
        {
            builder
                .AddConsole()
                .SetMinimumLevel(LogLevel.Debug);
        });

        ILogger logger = loggerFactory.CreateLogger("FoundryLocalSdkTest");

        // FOUNDRY_TEST_DATA_DIR is required for model-dependent tests.
        var envCacheDir = Environment.GetEnvironmentVariable("FOUNDRY_TEST_DATA_DIR");
        if (string.IsNullOrWhiteSpace(envCacheDir))
        {
            logger.LogWarning(
                "FOUNDRY_TEST_DATA_DIR is not set. Integration tests will be skipped.");
            Console.Error.WriteLine(
                "[Utils::Utils] FOUNDRY_TEST_DATA_DIR is not set. Integration tests will be skipped.");
            return;
        }

        string testDataSharedPath = Path.GetFullPath(envCacheDir);
        logger.LogInformation(
            "Using test model cache directory: {TestDataSharedPath}",
            testDataSharedPath);

        if (!Directory.Exists(testDataSharedPath))
        {
            logger.LogWarning(
                "Test model cache directory does not exist: {TestDataSharedPath}. Integration tests will be skipped. See LOCAL_MODEL_TESTING.md.",
                testDataSharedPath);
            Console.Error.WriteLine(
                $"[Utils::Utils] Test model cache directory does not exist: {testDataSharedPath}. Integration tests will be skipped.");
            return;
        }

        // Echo to stdout as well so CI test-output capture (which doesn't always
        // forward the ILogger console sink from an assembly-hook context) records
        // exactly which path we resolved. Critical when diagnosing initialization
        // failures from CI logs only.
        Console.WriteLine($"[Utils::Utils] FOUNDRY_TEST_DATA_DIR env: '{envCacheDir}'");
        Console.WriteLine($"[Utils::Utils] Resolved test model cache: '{testDataSharedPath}'");

        var config = new Configuration
        {
            AppName = "FoundryLocalSdkTest",
            LogLevel = Local.LogLevel.Debug,
            Web = new Configuration.WebService
            {
                Urls = "http://127.0.0.1:0"
            },
            ModelCacheDir = testDataSharedPath,
            LogsDir = Path.Combine(GetRepoRoot(), "sdk_v2", "cs", "logs"),
            // Leave as default. Currently that's a static catalog.
            //CatalogUrls = new List<(string Url, string? Filter)>
            //{
            //    ("https://ai.azure.com/api/eastus/ux/v1.0", null)
            //}
        };

        // Initialize the singleton instance.
        // Wrapped in Task.Run so the async work runs on a thread-pool thread with no captured
        // SynchronizationContext. Calling GetAwaiter().GetResult() directly here would deadlock
        // if any test runner or hosting context installs a single-threaded sync context (the
        // continuation would wait for this thread, which is blocked waiting for the result).
        try
        {
            Task.Run(() => FoundryLocalManager.CreateAsync(config, logger)).GetAwaiter().GetResult();
            IntegrationTestsAvailable = true;

            // Dump catalog information for debugging
            DumpCatalogInfo(logger);
        }
        catch (Exception ex)
        {
            // Surface the full exception detail to stdout/stderr so CI captures it. The
            // BeforeAssemblyException wrapper only echoes the top-level message, which on
            // FoundryLocalException is just "Error during initialization" — useless without
            // the inner native error / stack.
            Console.Error.WriteLine($"[Utils::Utils] FoundryLocalManager.CreateAsync failed: {ex}");
            logger.LogWarning(ex, "FoundryLocalManager.CreateAsync failed; integration tests will be skipped.");

            // DllNotFoundException (HRESULT 0x8007007E) is ambiguous — it fires when
            // foundry_local itself is missing OR when any of its transitive deps is missing.
            // Dump the test output dir so we can see which it is from CI logs.
            DumpNativeBinaryLayout(logger);

            // Don't rethrow: a manager-init failure must not abort the whole assembly.
            // IntegrationTestsAvailable stays false; integration tests skip via
            // [SkipUnlessIntegration]; pure unit tests still run.
        }
    }

    [Before(Assembly)]
    public static void AssemblyInit(AssemblyHookContext _)
    {
        // this is to ensure the static ctor is called
        // there's also a path via SkipUnlessIntegrationAttribute that inits it for some tests not all
        Console.WriteLine("AssemblyInit: IntegrationTestsAvailable = " + IntegrationTestsAvailable);
    }

    /// <summary>
    /// Dumps catalog information showing all model aliases and their variants with ID and task.
    /// Useful for debugging to see what models are available in the test environment.
    /// </summary>
    private static void DumpCatalogInfo(ILogger logger)
    {
        try
        {
            var manager = FoundryLocalManager.Instance;
            var catalog = manager.GetCatalogAsync().GetAwaiter().GetResult();
            var models = catalog.ListModelsAsync().GetAwaiter().GetResult();

            logger.LogInformation("=== Model Catalog Information ===");
            logger.LogInformation($"Total models: {models.Count}");
            Console.WriteLine($"[Utils] Model Catalog: {models.Count} models available");

            // Group models by alias
            var modelsByAlias = models.GroupBy(m => m.Alias ?? m.Id)
                                     .OrderBy(g => g.Key);

            foreach (var group in modelsByAlias)
            {
                logger.LogInformation($"\nAlias: {group.Key}");
                Console.WriteLine($"[Utils]   Alias: {group.Key}");

                foreach (var variant in group.OrderBy(m => m.Id))
                {
                    logger.LogInformation($"  - ID: {variant.Id}");
                    logger.LogInformation($"    Task: {variant.Info.Task}");
                    logger.LogInformation($"    Device: {variant.Info.Runtime?.DeviceType}, EP: {variant.Info.Runtime?.ExecutionProvider}");

                    Console.WriteLine($"[Utils]     - ID: {variant.Id}");
                    Console.WriteLine($"[Utils]       Task: {variant.Info.Task}");
                    Console.WriteLine($"[Utils]       Device: {variant.Info.Runtime?.DeviceType}, EP: {variant.Info.Runtime?.ExecutionProvider}");
                }
            }

            logger.LogInformation("=== End Model Catalog ===");
            Console.WriteLine("[Utils] === End Model Catalog ===");
        }
        catch (Exception ex)
        {
            logger.LogWarning(ex, "Failed to dump catalog information");
            Console.Error.WriteLine($"[Utils] Failed to dump catalog info: {ex.Message}");
        }
    }

    /// <summary>
    /// Diagnostic helper: log the layout of native binaries around AppContext.BaseDirectory
    /// so a DllNotFoundException in CI tells us exactly what's present vs. missing.
    /// </summary>
    private static void DumpNativeBinaryLayout(ILogger logger)
    {
        try
        {
            var baseDir = AppContext.BaseDirectory;
            Console.Error.WriteLine($"[AssemblyInit] AppContext.BaseDirectory: {baseDir}");

            void DumpDir(string label, string dir)
            {
                if (!Directory.Exists(dir))
                {
                    Console.Error.WriteLine($"[AssemblyInit] {label}: <missing> ({dir})");
                    return;
                }

                Console.Error.WriteLine($"[AssemblyInit] {label}: {dir}");
                var files = Directory.EnumerateFiles(dir, "*.*", SearchOption.TopDirectoryOnly)
                                     .Where(f => f.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) ||
                                                 f.EndsWith(".so", StringComparison.OrdinalIgnoreCase) ||
                                                 f.EndsWith(".dylib", StringComparison.OrdinalIgnoreCase) ||
                                                 f.EndsWith(".cfg", StringComparison.OrdinalIgnoreCase))
                                     .OrderBy(f => f);
                foreach (var f in files)
                {
                    Console.Error.WriteLine($"  {Path.GetFileName(f)}");
                }
            }

            DumpDir("baseDir native files", baseDir);

            var os = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "win" :
                     RuntimeInformation.IsOSPlatform(OSPlatform.Linux) ? "linux" :
                     RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? "osx" : "unknown";
            var arch = RuntimeInformation.OSArchitecture.ToString().ToLowerInvariant();
            DumpDir($"runtimes/{os}-{arch}/native", Path.Combine(baseDir, "runtimes", $"{os}-{arch}", "native"));

            var cfgPath = Path.Combine(baseDir, "foundry_local.native.cfg");
            if (File.Exists(cfgPath))
            {
                Console.Error.WriteLine($"[AssemblyInit] foundry_local.native.cfg contents: {File.ReadAllText(cfgPath).Trim()}");
            }
            else
            {
                Console.Error.WriteLine($"[AssemblyInit] foundry_local.native.cfg: <not present>");
            }
        }
        catch (Exception dumpEx)
        {
            Console.Error.WriteLine($"[AssemblyInit] DumpNativeBinaryLayout failed: {dumpEx}");
        }
    }

    internal static bool IsRunningInCI()
    {
        var azureDevOps = Environment.GetEnvironmentVariable("TF_BUILD");
        var githubActions = Environment.GetEnvironmentVariable("GITHUB_ACTIONS");
        var ci = Environment.GetEnvironmentVariable("CI");
        var isCI = string.Equals(azureDevOps, "True", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(githubActions, "true", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(ci, "true", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(ci, "1", StringComparison.OrdinalIgnoreCase);

        return isCI;
    }

    private static List<ModelInfo> BuildTestCatalog(bool includeCuda = true)
    {
        // Mirrors MOCK_CATALOG_DATA ordering and fields (Python tests)
        var common = new
        {
            ProviderType = "AzureFoundry",
            Version = 1,
            ModelType = "ONNX",
            PromptTemplate = (PromptTemplate?)null,
            Publisher = "Microsoft",
            Task = "chat-completion",
            FileSizeMb = 10403,
            ModelSettings = new ModelSettings { Parameters = [] },
            SupportsToolCalling = false,
            License = "MIT",
            LicenseDescription = "License…",
            MaxOutputTokens = 1024L,
            MinFLVersion = "1.0.0",
        };

        var list = new List<ModelInfo>
            {
                // model-1 generic-gpu, generic-cpu:2, generic-cpu:1
                new()
                {
                    Id = "model-1-generic-gpu:1",
                    Name = "model-1-generic-gpu",
                    DisplayName = "model-1-generic-gpu",
                    Uri = "azureml://registries/azureml/models/model-1-generic-gpu/versions/1",
                    Runtime = new Runtime { DeviceType = DeviceType.GPU, ExecutionProvider = "WebGpuExecutionProvider" },
                    Alias = "model-1",
                    // ParentModelUri = "azureml://registries/azureml/models/model-1/versions/1",
                    ProviderType = common.ProviderType, Version = common.Version, ModelType = common.ModelType,
                    PromptTemplate = common.PromptTemplate, Publisher = common.Publisher, Task = common.Task,
                    FileSizeMb = common.FileSizeMb, ModelSettings = common.ModelSettings,
                    SupportsToolCalling = common.SupportsToolCalling, License = common.License,
                    LicenseDescription = common.LicenseDescription, MaxOutputTokens = common.MaxOutputTokens,
                    MinFLVersion = common.MinFLVersion
                },
                new()
                {
                    Id = "model-1-generic-cpu:2",
                    Name = "model-1-generic-cpu",
                    DisplayName = "model-1-generic-cpu",
                    Uri = "azureml://registries/azureml/models/model-1-generic-cpu/versions/2",
                    Runtime = new Runtime { DeviceType = DeviceType.CPU, ExecutionProvider = "CPUExecutionProvider" },
                    Alias = "model-1",
                    // ParentModelUri = "azureml://registries/azureml/models/model-1/versions/2",
                    ProviderType = common.ProviderType,
                    Version = common.Version, ModelType = common.ModelType,
                    PromptTemplate = common.PromptTemplate,
                    Publisher = common.Publisher, Task = common.Task,
                    FileSizeMb = common.FileSizeMb - 10,  // smaller so default chosen in test that sorts on this
                    ModelSettings = common.ModelSettings,
                    SupportsToolCalling = common.SupportsToolCalling,
                    License = common.License,
                    LicenseDescription = common.LicenseDescription,
                    MaxOutputTokens = common.MaxOutputTokens,
                    MinFLVersion = common.MinFLVersion
                },
                new()
                {
                    Id = "model-1-generic-cpu:1",
                    Name = "model-1-generic-cpu",
                    DisplayName = "model-1-generic-cpu",
                    Uri = "azureml://registries/azureml/models/model-1-generic-cpu/versions/1",
                    Runtime = new Runtime { DeviceType = DeviceType.CPU, ExecutionProvider = "CPUExecutionProvider" },
                    Alias = "model-1",
                    //ParentModelUri = "azureml://registries/azureml/models/model-1/versions/1",
                    ProviderType = common.ProviderType,
                    Version = common.Version,
                    ModelType = common.ModelType,
                    PromptTemplate = common.PromptTemplate,
                    Publisher = common.Publisher, Task = common.Task,
                    FileSizeMb = common.FileSizeMb,
                    ModelSettings = common.ModelSettings,
                    SupportsToolCalling = common.SupportsToolCalling,
                    License = common.License,
                    LicenseDescription = common.LicenseDescription,
                    MaxOutputTokens = common.MaxOutputTokens,
                    MinFLVersion = common.MinFLVersion
                },

                // model-2 npu:2, npu:1, generic-cpu:1
                new()
                {
                    Id = "model-2-npu:2",
                    Name = "model-2-npu",
                    DisplayName = "model-2-npu",
                    Uri = "azureml://registries/azureml/models/model-2-npu/versions/2",
                    Runtime = new Runtime { DeviceType = DeviceType.NPU, ExecutionProvider = "QNNExecutionProvider" },
                    Alias = "model-2",
                    //ParentModelUri = "azureml://registries/azureml/models/model-2/versions/2",
                    ProviderType = common.ProviderType,
                    Version = common.Version, ModelType = common.ModelType,
                    PromptTemplate = common.PromptTemplate,
                    Publisher = common.Publisher, Task = common.Task,
                    FileSizeMb = common.FileSizeMb,
                    ModelSettings = common.ModelSettings,
                    SupportsToolCalling = common.SupportsToolCalling,
                    License = common.License,
                    LicenseDescription = common.LicenseDescription,
                    MaxOutputTokens = common.MaxOutputTokens,
                    MinFLVersion = common.MinFLVersion
                },
                new()
                {
                    Id = "model-2-npu:1",
                    Name = "model-2-npu",
                    DisplayName = "model-2-npu",
                    Uri = "azureml://registries/azureml/models/model-2-npu/versions/1",
                    Runtime = new Runtime { DeviceType = DeviceType.NPU, ExecutionProvider = "QNNExecutionProvider" },
                    Alias = "model-2",
                    //ParentModelUri = "azureml://registries/azureml/models/model-2/versions/1",
                    ProviderType = common.ProviderType,
                    Version = common.Version, ModelType = common.ModelType,
                    PromptTemplate = common.PromptTemplate,
                    Publisher = common.Publisher, Task = common.Task,
                    FileSizeMb = common.FileSizeMb,
                    ModelSettings = common.ModelSettings,
                    SupportsToolCalling = common.SupportsToolCalling,
                    License = common.License,
                    LicenseDescription = common.LicenseDescription,
                    MaxOutputTokens = common.MaxOutputTokens,
                    MinFLVersion = common.MinFLVersion
                },
                new()
                {
                    Id = "model-2-generic-cpu:1",
                    Name = "model-2-generic-cpu",
                    DisplayName = "model-2-generic-cpu",
                    Uri = "azureml://registries/azureml/models/model-2-generic-cpu/versions/1",
                    Runtime = new Runtime { DeviceType = DeviceType.CPU, ExecutionProvider = "CPUExecutionProvider" },
                    Alias = "model-2",
                    //ParentModelUri = "azureml://registries/azureml/models/model-2/versions/1",
                    ProviderType = common.ProviderType,
                    Version = common.Version, ModelType = common.ModelType,
                    PromptTemplate = common.PromptTemplate,
                    Publisher = common.Publisher, Task = common.Task,
                    FileSizeMb = common.FileSizeMb,
                    ModelSettings = common.ModelSettings,
                    SupportsToolCalling = common.SupportsToolCalling,
                    License = common.License,
                    LicenseDescription = common.LicenseDescription,
                    MaxOutputTokens = common.MaxOutputTokens,
                    MinFLVersion = common.MinFLVersion
                },
            };

        // model-3 cuda-gpu (optional), generic-gpu, generic-cpu
        if (includeCuda)
        {
            list.Add(new ModelInfo
            {
                Id = "model-3-cuda-gpu:1",
                Name = "model-3-cuda-gpu",
                DisplayName = "model-3-cuda-gpu",
                Uri = "azureml://registries/azureml/models/model-3-cuda-gpu/versions/1",
                Runtime = new Runtime { DeviceType = DeviceType.GPU, ExecutionProvider = "CUDAExecutionProvider" },
                Alias = "model-3",
                //ParentModelUri = "azureml://registries/azureml/models/model-3/versions/1",
                ProviderType = common.ProviderType,
                Version = common.Version,
                ModelType = common.ModelType,
                PromptTemplate = common.PromptTemplate,
                Publisher = common.Publisher,
                Task = common.Task,
                FileSizeMb = common.FileSizeMb,
                ModelSettings = common.ModelSettings,
                SupportsToolCalling = common.SupportsToolCalling,
                License = common.License,
                LicenseDescription = common.LicenseDescription,
                MaxOutputTokens = common.MaxOutputTokens,
                MinFLVersion = common.MinFLVersion
            });
        }

        list.AddRange(new[]
        {
                new ModelInfo
                {
                    Id = "model-3-generic-gpu:1",
                    Name = "model-3-generic-gpu",
                    DisplayName = "model-3-generic-gpu",
                    Uri = "azureml://registries/azureml/models/model-3-generic-gpu/versions/1",
                    Runtime = new Runtime { DeviceType = DeviceType.GPU, ExecutionProvider = "WebGpuExecutionProvider" },
                    Alias = "model-3",
                    //ParentModelUri = "azureml://registries/azureml/models/model-3/versions/1",
                    ProviderType = common.ProviderType,
                    Version = common.Version, ModelType = common.ModelType,
                    PromptTemplate = common.PromptTemplate,
                    Publisher = common.Publisher, Task = common.Task,
                    FileSizeMb = common.FileSizeMb,
                    ModelSettings = common.ModelSettings,
                    SupportsToolCalling = common.SupportsToolCalling,
                    License = common.License,
                    LicenseDescription = common.LicenseDescription,
                    MaxOutputTokens = common.MaxOutputTokens,
                    MinFLVersion = common.MinFLVersion
                },
                new ModelInfo
                {
                    Id = "model-3-generic-cpu:1",
                    Name = "model-3-generic-cpu",
                    DisplayName = "model-3-generic-cpu",
                    Uri = "azureml://registries/azureml/models/model-3-generic-cpu/versions/1",
                    Runtime = new Runtime { DeviceType = DeviceType.CPU, ExecutionProvider = "CPUExecutionProvider" },
                    Alias = "model-3",
                    //ParentModelUri = "azureml://registries/azureml/models/model-3/versions/1",
                    ProviderType = common.ProviderType,
                    Version = common.Version,
                    ModelType = common.ModelType,
                    PromptTemplate = common.PromptTemplate,
                    Publisher = common.Publisher, Task = common.Task,
                    FileSizeMb = common.FileSizeMb,
                    ModelSettings = common.ModelSettings,
                    SupportsToolCalling = common.SupportsToolCalling,
                    License = common.License,
                    LicenseDescription = common.LicenseDescription,
                    MaxOutputTokens = common.MaxOutputTokens,
                    MinFLVersion = common.MinFLVersion
                }
            });

        // model-4 generic-gpu (nullable prompt)
        list.Add(new ModelInfo
        {
            Id = "model-4-generic-gpu:1",
            Name = "model-4-generic-gpu",
            DisplayName = "model-4-generic-gpu",
            Uri = "azureml://registries/azureml/models/model-4-generic-gpu/versions/1",
            Runtime = new Runtime { DeviceType = DeviceType.GPU, ExecutionProvider = "WebGpuExecutionProvider" },
            Alias = "model-4",
            //ParentModelUri = "azureml://registries/azureml/models/model-4/versions/1",
            ProviderType = common.ProviderType,
            Version = common.Version,
            ModelType = common.ModelType,
            PromptTemplate = null,
            Publisher = common.Publisher,
            Task = common.Task,
            FileSizeMb = common.FileSizeMb,
            ModelSettings = common.ModelSettings,
            SupportsToolCalling = common.SupportsToolCalling,
            License = common.License,
            LicenseDescription = common.LicenseDescription,
            MaxOutputTokens = common.MaxOutputTokens,
            MinFLVersion = common.MinFLVersion
        });

        return list;
    }

    private static string GetSourceFilePath([CallerFilePath] string path = "") => path;

    // Gets the root directory of the foundry-local-sdk repository by finding the .git directory.
    // In a regular checkout `.git` is a directory; in a git worktree it's a file pointing to
    // the main repo's worktrees/ entry — accept either form.
    private static string GetRepoRoot()
    {
        var sourceFile = GetSourceFilePath();
        var dir = new DirectoryInfo(Path.GetDirectoryName(sourceFile)!);

        while (dir != null)
        {
            var gitPath = Path.Combine(dir.FullName, ".git");
            if (Directory.Exists(gitPath) || File.Exists(gitPath))
                return dir.FullName;

            dir = dir.Parent;
        }

        throw new InvalidOperationException("Could not find git repository root from test file location");
    }
}
