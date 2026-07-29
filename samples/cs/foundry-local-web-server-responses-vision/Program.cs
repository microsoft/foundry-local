// <complete_code>
// <imports>
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.AI.Foundry.Local;
// </imports>

const int DefaultMaxOutputTokens = 8192;

if (args.Length < 1)
{
    Console.Error.WriteLine("Usage: dotnet run -- <model_alias_or_id> [image_path]");
    Console.Error.WriteLine("         dotnet run -- --list-models");
    Console.Error.WriteLine("  Example: dotnet run -- qwen3.5-0.8b");
    Console.Error.WriteLine("  Example: dotnet run -- Qwen2.5-VL-7B-Instruct-generic-cpu");
    return 1;
}

bool listModels = args[0] is "--list-models" or "-l";
string? modelIdentifier = listModels ? null : args[0];
string defaultImage = Path.Combine(AppContext.BaseDirectory, "test_image.jpg");
string imagePath = !listModels && args.Length > 1 ? args[1] : defaultImage;

// <init>
var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information,
    Web = new Configuration.WebService
    {
        Urls = "http://127.0.0.1:52496"
    }
};

await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
var mgr = FoundryLocalManager.Instance;

Console.WriteLine("\nDownloading execution providers:");
var currentEp = "";
await mgr.DownloadAndRegisterEpsAsync((epName, percent) =>
{
    if (epName != currentEp)
    {
        if (currentEp != "") Console.WriteLine();
        currentEp = epName;
    }
    Console.Write($"\r  {epName.PadRight(30)}  {percent,6:F1}%");
});
if (currentEp != "") Console.WriteLine();
// </init>

var catalog = await mgr.GetCatalogAsync();

if (listModels)
{
    var allModels = await catalog.ListModelsAsync();
    var visionModels = allModels
        .Where(m => (m.Info?.Task ?? "").Contains("vision", StringComparison.OrdinalIgnoreCase))
        .OrderBy(m => m.Alias)
        .ToList();

    if (visionModels.Count == 0)
    {
        Console.WriteLine("\nNo vision models found in catalog.");
        return 0;
    }

    var totalVariants = visionModels.Sum(m => m.Variants.Count);
    Console.WriteLine($"\nVision models in catalog ({visionModels.Count} aliases, {totalVariants} variants):");
    Console.WriteLine($"  {"ALIAS",-32}  {"INPUT MODALITIES",-20}  {"OUTPUT MODALITIES",-20}  {"TASK",-24}  CAPABILITIES");
    foreach (var m in visionModels)
    {
        var task = m.Info?.Task ?? "";
        var capabilities = m.Info?.Capabilities ?? "";
        var inMod = m.Info?.InputModalities ?? "";
        var outMod = m.Info?.OutputModalities ?? "";
        Console.WriteLine($"  {m.Alias,-32}  {inMod,-20}  {outMod,-20}  {task,-24}  {capabilities}");

        var variants = m.Variants
            .OrderBy(v => v.Info?.Runtime?.DeviceType.ToString() ?? "")
            .ThenBy(v => v.Info?.Runtime?.ExecutionProvider ?? "")
            .ThenBy(v => v.Id)
            .ToList();
        if (variants.Count == 0) continue;

        Console.WriteLine($"      {"VARIANT ID",-54}  {"DEVICE",-6}  {"EXECUTION PROVIDER",-32}  {"SIZE (MB)",10}  CACHED");
        foreach (var v in variants)
        {
            var rt = v.Info?.Runtime;
            var device = rt?.DeviceType.ToString() ?? "";
            var ep = rt?.ExecutionProvider ?? "";
            var size = v.Info?.FileSizeMb is int s ? s.ToString().PadLeft(10) : new string(' ', 10);
            var cached = await v.IsCachedAsync() ? "yes" : "no";
            Console.WriteLine($"      {v.Id,-54}  {device,-6}  {ep,-32}  {size}  {cached}");
        }
    }
    return 0;
}

// <model_setup>
var model = await catalog.GetModelAsync(modelIdentifier!);
if (model is null)
{
    model = await catalog.GetModelVariantAsync(modelIdentifier!);
}
if (model is null)
{
    var available = (await catalog.ListModelsAsync()).Select(m => m.Alias);
    Console.Error.WriteLine($"\nModel '{modelIdentifier}' not found in catalog (tried alias and variant id).");
    Console.Error.WriteLine($"Available aliases: {string.Join(", ", available)}");
    Console.Error.WriteLine("Run with --list-models to see variant ids.");
    return 1;
}

if (!await model.IsCachedAsync())
{
    Console.WriteLine($"\nDownloading model {modelIdentifier}...");
    await model.DownloadAsync(progress =>
    {
        Console.Write($"\rDownloading model: {progress:F2}%");
        if (progress >= 100f) Console.WriteLine();
    });
    Console.WriteLine("Model downloaded");
}

Console.WriteLine("\nLoading model...");
await model.LoadAsync();
Console.WriteLine("Model loaded");
// </model_setup>

// <server_setup>
Console.WriteLine("\nStarting web service...");
await mgr.StartWebServiceAsync();
var baseUrl = config.Web.Urls!.TrimEnd('/') + "/v1";
Console.WriteLine($"Web service started on {baseUrl}");
// </server_setup>

// <inference>
Console.WriteLine($"\nPreparing image: {imagePath}");
var (imageB64, mediaType) = EncodeImage(imagePath);

// The Foundry Local Responses API accepts an array of message items with input_text /
// input_image content parts. The input_image part uses Foundry-specific `image_data` and
// `media_type` fields (in place of OpenAI's `image_url`).
var visionInput = new JsonArray
{
    new JsonObject
    {
        ["type"] = "message",
        ["role"] = "user",
        ["content"] = new JsonArray
        {
            new JsonObject { ["type"] = "input_text", ["text"] = "Describe this image." },
            new JsonObject
            {
                ["type"] = "input_image",
                ["image_data"] = imageB64,
                ["media_type"] = mediaType,
            },
        },
    },
};

var body = new JsonObject
{
    ["model"] = model.Id,
    ["input"] = visionInput,
    ["max_output_tokens"] = DefaultMaxOutputTokens,
    ["stream"] = true,
};

using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
using var request = new HttpRequestMessage(HttpMethod.Post, $"{baseUrl}/responses")
{
    Content = new StringContent(body.ToJsonString(), Encoding.UTF8, "application/json"),
};
request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("text/event-stream"));
request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", "notneeded");

Console.WriteLine("\nStreaming vision response...");
using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
response.EnsureSuccessStatusCode();

await using var stream = await response.Content.ReadAsStreamAsync();
using var reader = new StreamReader(stream);

Console.Write("[ASSISTANT]: ");
while (await reader.ReadLineAsync() is string line)
{
    if (string.IsNullOrEmpty(line) || !line.StartsWith("data: ", StringComparison.Ordinal))
        continue;
    var data = line["data: ".Length..];
    if (data == "[DONE]") break;
    try
    {
        using var doc = JsonDocument.Parse(data);
        var root = doc.RootElement;
        if (root.TryGetProperty("type", out var t) &&
            t.GetString() == "response.output_text.delta" &&
            root.TryGetProperty("delta", out var d))
        {
            Console.Write(d.GetString());
        }
    }
    catch (JsonException) { /* ignore non-JSON keepalives */ }
}
Console.WriteLine();
// </inference>

await mgr.StopWebServiceAsync();
await model.UnloadAsync();
return 0;

static (string Base64, string MediaType) EncodeImage(string path)
{
    var mediaTypes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
    {
        [".jpg"] = "image/jpeg",
        [".jpeg"] = "image/jpeg",
        [".png"] = "image/png",
        [".gif"] = "image/gif",
        [".bmp"] = "image/bmp",
        [".webp"] = "image/webp",
    };
    var ext = Path.GetExtension(path);
    var mediaType = mediaTypes.TryGetValue(ext, out var m) ? m : "image/jpeg";
    var bytes = File.ReadAllBytes(path);
    return (Convert.ToBase64String(bytes), mediaType);
}
// </complete_code>
