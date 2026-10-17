// Foundry Local SDK v2 - BYOM Registration E2E Demo
//
// Exercises the full lifecycle for a BYOM registered *while Foundry Local
// is already running*: discovery, load, streaming chat completion, unload,
// cleanup.
//
// Flow:
//   1. Initialize Foundry Local against its default cache directory.
//   2. Resolve and (if needed) download a small catalog model (qwen2.5-0.5b).
//   3. While the manager is still running, clone its on-disk directory into
//      a sibling under the same publisher, rewriting inference_model.json's
//      Name field with a fresh unique alias. The bulk files are hardlinked
//      where possible so the clone is cheap.
//   4. Register the new BYOM in CatalogType.Local, then look it up by alias.
//   5. Load it, stream a chat completion, then unload.
//   6. Always delete the cloned directory in a finally block.
//
// SDK v2 uses explicit local registration, not public-catalog cache rescans.
//
// Usage:
//   dotnet run

using Microsoft.AI.Foundry.Local;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

const string PASS = "\u001b[92m[PASS]\u001b[0m";
const string INFO = "\u001b[94m[INFO]\u001b[0m";
const string WARN = "\u001b[93m[WARN]\u001b[0m";
const string FIX_ACTIVE = "\u001b[92m[ FIX ACTIVE ]\u001b[0m";

const string SourceAlias = "qwen2.5-0.5b";
string byomAlias = "fl-rescan-e2e-" + DateTime.Now.ToString("yyyyMMdd-HHmmss");
const string ByomSubdir = "byom_rescan_e2e";
const string Prompt = "What is the golden ratio? Answer in one short sentence.";

void Section(string title)
{
    Console.WriteLine();
    Console.WriteLine(new string('=', 72));
    Console.WriteLine($"  {title}");
    Console.WriteLine(new string('=', 72));
}

CancellationToken ct = CancellationToken.None;
Console.WriteLine($"{INFO} BYOM alias for this run: {byomAlias}");

var config = new Configuration
{
    AppName = "foundry_local_byom_rescan_e2e",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Warning,
};

await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
using var mgr = FoundryLocalManager.Instance;

Section("Step 1: Register execution providers");
var eps = mgr.DiscoverEps();
if (eps.Length > 0)
{
    string currentEp = "";
    await mgr.DownloadAndRegisterEpsAsync((epName, percent) =>
    {
        if (epName != currentEp)
        {
            if (currentEp != "")
            {
                Console.WriteLine();
            }
            currentEp = epName;
        }
        Console.Write($"\r  EP [{epName,-24}] {percent,6:F2}%");
    });
    Console.WriteLine();
}
else
{
    Console.WriteLine("  No execution providers to download.");
}

Section($"Step 2: Resolve source catalog model '{SourceAlias}'");
var catalog = await mgr.GetCatalogAsync();
var localCatalog = await mgr.GetCatalogAsync(CatalogType.Local);
var source = await catalog.GetModelAsync(SourceAlias, ct);
if (source is null)
{
    Console.WriteLine($"  {WARN} {SourceAlias} not found in catalog; aborting.");
    return 1;
}
var cpuVariant = source.Variants.FirstOrDefault(
    variant => variant.Info?.Runtime?.ExecutionProvider == "CPUExecutionProvider")
    ?? throw new InvalidOperationException("The BYOM demo requires a CPU execution-provider variant.");
source.SelectVariant(cpuVariant);
Console.WriteLine($"  resolved to variant id: {source.Id}");

if (!await source.IsCachedAsync(ct))
{
    Console.WriteLine($"  {INFO} source not cached; downloading (one-time)...");
    await source.DownloadAsync(progress =>
    {
        Console.Write($"\r  download: {progress,6:F2}%");
        if (progress >= 100f) Console.WriteLine();
    });
}
else
{
    Console.WriteLine($"  {INFO} source already cached -- no download needed.");
}

string srcPath = await source.GetPathAsync(ct);
Console.WriteLine($"  source path: {srcPath}");
if (!File.Exists(Path.Combine(srcPath, "inference_model.json")))
{
    Console.WriteLine($"  {WARN} {srcPath} has no inference_model.json; aborting.");
    return 1;
}

// Layout: <cache_root>/<publisher>/<model_dir>/<version>. Place the clone as
// a sibling of the model_dir, under a grouping subdir for tidy cleanup.
string publisherDir = Directory.GetParent(srcPath)?.Parent?.FullName
                      ?? throw new InvalidOperationException("Cannot resolve publisher dir.");
string cloneDir = Path.Combine(publisherDir, ByomSubdir, byomAlias);

Section("Step 3: Clone source model into a new BYOM directory (mid-session)");
Console.WriteLine($"  cloning -> {cloneDir}");
var t0 = DateTime.UtcNow;
CloneModelDir(srcPath, cloneDir, byomAlias);
Console.WriteLine($"  clone done in {(DateTime.UtcNow - t0).TotalSeconds:F2}s");

int exitCode = 0;
IModel? byom = null;
var registered = false;
try
{
    Section($"Step 4: Register and discover BYOM '{byomAlias}' in the local catalog");
    var tGet = DateTime.UtcNow;
    await localCatalog.RegisterModelAsync(cloneDir, $"{byomAlias}:1",
        new ModelInfoBuilder().SetStringProperty(ModelInfoPropertyKeys.Task, "chat-completion"), ct);
    registered = true;
    byom = await localCatalog.GetModelAsync(byomAlias, ct);
    Console.WriteLine($"  catalog.GetModelAsync returned in {(DateTime.UtcNow - tGet).TotalSeconds:F2}s");
    if (byom is null)
    {
        throw new InvalidOperationException("Registered BYOM was not found in the local catalog.");
    }
    Console.WriteLine($"  {FIX_ACTIVE} resolved {byom.Id}  (alias={byom.Alias})");

    var cached = await localCatalog.GetCachedModelsAsync(ct);
    var cachedIds = cached.Select(m => m.Id).ToList();
    if (!cachedIds.Any(id => id.Contains(byomAlias)))
    {
        Console.WriteLine($"  {WARN} BYOM resolved by GetModelAsync but missing "
            + $"from GetCachedModelsAsync(): [{string.Join(", ", cachedIds)}]");
    }
    else
    {
        Console.WriteLine($"  {PASS} BYOM also visible in GetCachedModelsAsync()");
    }

    Section("Step 5: Load the BYOM and stream a chat completion");
    var tLoad = DateTime.UtcNow;
    await byom.LoadAsync();
    Console.WriteLine($"  LoadAsync() returned in {(DateTime.UtcNow - tLoad).TotalSeconds:F2}s");

    Console.WriteLine();
    Console.WriteLine($"  prompt: {Prompt}");
    Console.Write("  Assistant: ");
    bool anyContent = false;
    using (var session = new ChatSession(byom))
    {
        session.SetStreaming(true);
        using var request = new Request();
        request.AddItem(MessageItem.User(Prompt));
        await using var stream = session.ProcessStreamingRequestAsync(request, ct);
        await foreach (var item in stream)
        {
            using (item)
            {
                if (item is TextItem { Type: TextItemType.Default } text)
                {
                    anyContent |= text.Text.Length > 0;
                    Console.Write(text.Text);
                    Console.Out.Flush();
                }
            }
        }
    }
    Console.WriteLine();
    Console.WriteLine(anyContent
        ? $"  {PASS} streaming chat completion succeeded."
        : $"  {WARN} streaming completed with no content.");

    Section("Step 6: Unload the BYOM");
    await byom.UnloadAsync();
    byom = null;
    Console.WriteLine($"  {PASS} unloaded.");

    Section("Result");
    Console.WriteLine($"  {FIX_ACTIVE} BYOM added mid-session was discovered, "
        + "loaded, ran, and unloaded successfully.");
    return 0;
}
catch (Exception ex)
{
    Console.WriteLine();
    Console.WriteLine($"  {WARN} unexpected error during E2E: {ex}");
    exitCode = 3;
    return exitCode;
}
finally
{
    // Best-effort unload only if the happy-path unload did not run.
    if (byom is not null)
    {
        try { await byom.UnloadAsync(); } catch { /* best-effort */ }
    }
    if (registered)
    {
        await localCatalog.UnregisterModelAsync(byomAlias, ct);
    }
    Console.WriteLine();
    Console.WriteLine($"{INFO} cleaning up cloned BYOM at {cloneDir}");
    try
    {
        if (Directory.Exists(cloneDir)) Directory.Delete(cloneDir, recursive: true);
        string groupDir = Path.Combine(publisherDir, ByomSubdir);
        if (Directory.Exists(groupDir) && !Directory.EnumerateFileSystemEntries(groupDir).Any())
        {
            Directory.Delete(groupDir);
        }
    }
    catch (Exception ex)
    {
        Console.WriteLine($"  {WARN} cleanup failed: {ex.Message}");
    }
    Console.WriteLine($"{INFO} done (exit code {exitCode}).");
}

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

// Mirror src into dst using hardlinks where supported (cheap for ONNX
// weights), falling back to a regular copy otherwise. Then rewrite
// inference_model.json in dst to advertise newName. The hardlink for
// inference_model.json is unlinked from dst before writing so the original
// file is never mutated.
static void CloneModelDir(string src, string dst, string newName)
{
    Directory.CreateDirectory(dst);
    CopyTreeWithHardlinks(src, dst);

    string infPath = Path.Combine(dst, "inference_model.json");
    if (!File.Exists(infPath))
    {
        throw new FileNotFoundException(
            $"Expected {infPath} to exist after clone; source model dir does "
            + "not appear to be a Foundry Local cached model.");
    }

    string original = File.ReadAllText(Path.Combine(src, "inference_model.json"));
    var node = JsonNode.Parse(original) as JsonObject
               ?? throw new InvalidOperationException("inference_model.json is not a JSON object.");
    node["Name"] = newName;

    // Break the hardlink before mutating so the source file stays intact.
    File.Delete(infPath);
    File.WriteAllText(infPath, node.ToJsonString());
}

static void CopyTreeWithHardlinks(string src, string dst)
{
    Directory.CreateDirectory(dst);
    foreach (var entry in Directory.EnumerateFileSystemEntries(src))
    {
        string name = Path.GetFileName(entry);
        string target = Path.Combine(dst, name);
        if (Directory.Exists(entry))
        {
            CopyTreeWithHardlinks(entry, target);
        }
        else if (!TryHardlink(entry, target))
        {
            File.Copy(entry, target);
        }
    }
}

static bool TryHardlink(string src, string dst)
{
    try
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            return CreateHardLinkW(dst, src, IntPtr.Zero);
        }
        // POSIX: use the unix link syscall via Mono.Posix is not available
        // by default; rely on File.Copy fallback off-Windows. Hardlinks are
        // a perf optimization only -- correctness does not depend on them.
        return false;
    }
    catch
    {
        return false;
    }
}

[DllImport("kernel32.dll", EntryPoint = "CreateHardLinkW",
           CharSet = CharSet.Unicode, SetLastError = true)]
static extern bool CreateHardLinkW(
    string lpFileName, string lpExistingFileName, IntPtr lpSecurityAttributes);
