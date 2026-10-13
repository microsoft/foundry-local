/// <summary>
/// Foundry Local SDK - WinML 2.0 EP Verification (C#)
///
/// Verifies:
///   1. Execution providers are discovered and registered
///   2. Accelerated models appear in catalog after EP registration
///   3. Streaming chat completions work on an accelerated model
/// </summary>

using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging;
using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;

const string PASS = "\x1b[92m[PASS]\x1b[0m";
const string FAIL = "\x1b[91m[FAIL]\x1b[0m";
const string INFO = "\x1b[94m[INFO]\x1b[0m";
const string WARN = "\x1b[93m[WARN]\x1b[0m";

var results = new List<(string Name, bool Passed)>();

void LogResult(string testName, bool passed, string detail = "")
{
    var status = passed ? PASS : FAIL;
    var msg = string.IsNullOrEmpty(detail) ? $"{status} {testName}" : $"{status} {testName} - {detail}";
    Console.WriteLine(msg);
    results.Add((testName, passed));
}

void PrintSeparator(string title)
{
    Console.WriteLine($"\n{new string('=', 60)}");
    Console.WriteLine($"  {title}");
    Console.WriteLine($"{new string('=', 60)}\n");
}

void PrintSummary()
{
    PrintSeparator("Summary");
    var passed = results.Count(r => r.Passed);
    foreach (var (name, p) in results)
    {
        Console.WriteLine($"  {(p ? "✓" : "✗")} {name}");
    }

    Console.WriteLine($"\n  {passed}/{results.Count} tests passed");
}

bool IsAcceleratedVariant(IModel model)
{
    var runtime = model.Info?.Runtime;
    return runtime != null && (runtime.DeviceType == DeviceType.GPU || runtime.DeviceType == DeviceType.NPU);
}

CancellationToken ct = CancellationToken.None;

// ── 0. Initialize FoundryLocalManager ──────────────────────
PrintSeparator("Initialization");
var config = new Configuration
{
    AppName = "verify_winml",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
};

using var loggerFactory = LoggerFactory.Create(builder =>
    builder.SetMinimumLevel(Microsoft.Extensions.Logging.LogLevel.Information));
var logger = loggerFactory.CreateLogger<Program>();

await FoundryLocalManager.CreateAsync(config, logger);
var mgr = FoundryLocalManager.Instance;
Console.WriteLine($"{INFO} FoundryLocalManager initialized.");

// ── 1. Discover & Register EPs ────────────────────────────
PrintSeparator("Step 1: Discover & Register Execution Providers");
EpInfo[] eps = [];
try
{
    eps = mgr.DiscoverEps();
    Console.WriteLine($"{INFO} Discovered {eps.Length} execution providers:");
    foreach (var ep in eps)
    {
        Console.WriteLine($"  - {ep.Name,-40}  Registered: {ep.IsRegistered}");
    }

    LogResult("EP Discovery", true, $"{eps.Length} EP(s) found");
}
catch (Exception e)
{
    LogResult("EP Discovery", false, e.Message);
}

if (eps.Length == 0)
{
    var detail = "No execution providers discovered on this machine";
    LogResult("EP Download & Registration", false, detail);
    Console.WriteLine($"\n{FAIL} {detail}.");
    PrintSummary();
    return;
}

try
{
    string? currentProgressEp = null;
    var currentProgressPercent = -1d;

    var epResult = await mgr.DownloadAndRegisterEpsAsync(
        new Action<string, double>((epName, percent) =>
        {
            if (currentProgressEp != null &&
                (!epName.Equals(currentProgressEp, StringComparison.OrdinalIgnoreCase) || percent < currentProgressPercent))
            {
                Console.WriteLine();
            }

            currentProgressEp = epName;
            currentProgressPercent = percent;
            Console.Write($"\r  Downloading {epName}: {percent:F1}%");
        }),
        ct);

    if (currentProgressEp != null)
    {
        Console.WriteLine();
    }

    Console.WriteLine($"{INFO} EP registration: success={epResult.Success}, status={epResult.Status}");
    if (epResult.RegisteredEps?.Any() == true)
    {
        Console.WriteLine($"  Registered: {string.Join(", ", epResult.RegisteredEps)}");
    }

    if (epResult.FailedEps?.Any() == true)
    {
        Console.WriteLine($"  Failed:     {string.Join(", ", epResult.FailedEps)}");
    }

    var downloadOk = epResult.Success;
    var detail = downloadOk && epResult.RegisteredEps?.Any() == true
        ? $"{epResult.RegisteredEps.Length} EP(s) registered"
        : epResult.Status;
    LogResult("EP Download & Registration", downloadOk, detail);
    if (!downloadOk)
    {
        PrintSummary();
        return;
    }
}
catch (Exception e)
{
    Console.WriteLine();
    LogResult("EP Download & Registration", false, e.Message);
    PrintSummary();
    return;
}

// ── 2. List Models & Find Accelerated Variants ────────────
PrintSeparator("Step 2: Model Catalog - Accelerated Models");
var catalog = await mgr.GetCatalogAsync();
var models = await catalog.ListModelsAsync();
Console.WriteLine($"{INFO} Total models in catalog: {models.Count}");

var acceleratedVariants = new List<IModel>();
foreach (var model in models)
{
    foreach (var variant in model.Variants)
    {
        if (IsAcceleratedVariant(variant))
        {
            acceleratedVariants.Add(variant);
            var runtime = variant.Info?.Runtime;
            Console.WriteLine($"  - {variant.Id,-50}  Device: {runtime?.DeviceType,-3}  EP: {runtime?.ExecutionProvider ?? "?"}");
        }
    }
}

LogResult("Catalog - Accelerated models found", acceleratedVariants.Count > 0,
    acceleratedVariants.Count > 0 ? $"{acceleratedVariants.Count} accelerated variant(s)" : "No accelerated model variants");

if (acceleratedVariants.Count == 0)
{
    Console.WriteLine($"\n{FAIL} No accelerated model variants are available.");
    Console.WriteLine($"{WARN} Ensure the system has a compatible accelerator and matching model variants installed.");
    PrintSummary();
    return;
}

// ── 3. Download & Load Model ──────────────────────────────
PrintSeparator("Step 3: Download & Load Model");
IModel? chosen = null;
Exception? lastLoadError = null;
var downloadedAny = false;

foreach (var candidate in acceleratedVariants)
{
    var ep = candidate.Info?.Runtime?.ExecutionProvider ?? "unknown";
    Console.WriteLine($"\n{INFO} Trying model: {candidate.Id} (EP: {ep})");

    try
    {
        await candidate.DownloadAsync(progress =>
            Console.Write($"\r  Downloading model: {progress:F1}%"));
        Console.WriteLine();
        downloadedAny = true;
    }
    catch (Exception e)
    {
        Console.WriteLine();
        Console.WriteLine($"{WARN} Skipping {candidate.Id}: download failed: {e.Message}");
        lastLoadError = e;
        continue;
    }

    try
    {
        await candidate.LoadAsync();
        chosen = candidate;
        break;
    }
    catch (Exception e)
    {
        Console.WriteLine($"{WARN} Skipping {candidate.Id}: load failed: {e.Message}");
        lastLoadError = e;
    }
}

LogResult("Model Download", downloadedAny,
    downloadedAny ? "At least one accelerated variant downloaded" : lastLoadError?.Message ?? "No accelerated variant could be downloaded");

if (chosen == null)
{
    LogResult("Model Load", false,
        lastLoadError?.Message ?? "No accelerated variant could be loaded on this machine");
    PrintSummary();
    return;
}

LogResult("Model Load", true, $"Loaded {chosen.Id}");

// ── 4. Streaming Chat Completions (Native SDK) ────────────
PrintSeparator("Step 4: Streaming Chat Completions (Native)");
try
{
    var chatClient = await chosen.GetChatClientAsync();
    chatClient.Settings.Temperature = 0;
    chatClient.Settings.MaxTokens = 500;
    var messages = new List<ChatMessage>
    {
        new() { Role = "system", Content = "You are a helpful assistant." },
        new() { Role = "user", Content = "What is 2 + 2? Reply with just the number." },
    };

    var fullResponse = "";
    var start = DateTime.UtcNow;
    await foreach (var chunk in chatClient.CompleteChatStreamingAsync(messages, ct))
    {
        var content = chunk.Choices?.FirstOrDefault()?.Message?.Content;
        if (!string.IsNullOrEmpty(content))
        {
            Console.Write(content);
            Console.Out.Flush();
            fullResponse += content;
        }
    }

    var elapsed = (DateTime.UtcNow - start).TotalSeconds;
    Console.WriteLine();
    LogResult("Streaming Chat (Native)", fullResponse.Length > 0,
        $"{fullResponse.Length} chars in {elapsed:F2}s");
}
catch (Exception e)
{
    LogResult("Streaming Chat (Native)", false, e.Message);
}

// ── Summary ──────────────────────────────────────────────
PrintSummary();

await chosen.UnloadAsync();
Console.WriteLine("Model unloaded. Done!");
