// <complete_code>
// <imports>
using System.Text.Json;
using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging;
// </imports>

CancellationToken ct = CancellationToken.None;

// <tool_definitions>
// --- Tool definitions ---
const string weatherSchema = """
    {
      "type": "object",
      "properties": {
        "location": { "type": "string", "description": "The city or location" },
        "unit": { "type": "string", "enum": ["celsius", "fahrenheit"] }
      },
      "required": ["location"]
    }
    """;
const string calculateSchema = """
    {
      "type": "object",
      "properties": {
        "expression": { "type": "string", "description": "The math expression to evaluate" }
      },
      "required": ["expression"]
    }
    """;

// --- Tool implementations ---
string ExecuteTool(string functionName, JsonElement arguments)
{
    switch (functionName)
    {
        case "get_weather":
            var location = arguments.GetProperty("location")
                .GetString() ?? "unknown";
            var unit = arguments.TryGetProperty("unit", out var u)
                ? u.GetString() ?? "celsius"
                : "celsius";
            var temp = unit == "celsius" ? 22 : 72;
            return JsonSerializer.Serialize(new
            {
                location,
                temperature = temp,
                unit,
                condition = "Sunny"
            });

        case "calculate":
            var expression = arguments.GetProperty("expression")
                .GetString() ?? "";
            try
            {
                var result = new System.Data.DataTable()
                    .Compute(expression, null);
                return JsonSerializer.Serialize(new
                {
                    expression,
                    result = result?.ToString()
                });
            }
            catch (Exception ex)
            {
                return JsonSerializer.Serialize(new
                {
                    error = ex.Message
                });
            }

        default:
            return JsonSerializer.Serialize(new
            {
                error = $"Unknown function: {functionName}"
            });
    }
}
// </tool_definitions>

// <init>
// --- Main application ---
var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
};

using var loggerFactory = LoggerFactory.Create(builder =>
{
    builder.SetMinimumLevel(
        Microsoft.Extensions.Logging.LogLevel.Information
    );
});
var logger = loggerFactory.CreateLogger<Program>();

await FoundryLocalManager.CreateAsync(config, logger);
using var mgr = FoundryLocalManager.Instance;

// Download and register all execution providers.
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

var catalog = await mgr.GetCatalogAsync();
var model = await catalog.GetModelAsync("qwen2.5-0.5b")
    ?? throw new Exception("Model not found");

await model.DownloadAsync(progress =>
{
    Console.Write($"\rDownloading model: {progress:F2}%");
    if (progress >= 100f) Console.WriteLine();
});

await model.LoadAsync();
Console.WriteLine("Model loaded and ready.");

try
{
    using var session = new ChatSession(model);
    session.AddToolDefinition("get_weather", "Get example weather for a location (simulated data).", weatherSchema);
    session.AddToolDefinition("calculate", "Perform a math calculation.", calculateSchema);
    var firstTurn = true;
    // </init>

    // <tool_loop>
    Console.WriteLine("\nTool-calling assistant ready! Type 'quit' to exit.\n");

    while (true)
    {
        Console.Write("You: ");
        var userInput = Console.ReadLine();
        if (string.IsNullOrWhiteSpace(userInput) ||
            userInput.Equals("quit", StringComparison.OrdinalIgnoreCase) ||
            userInput.Equals("exit", StringComparison.OrdinalIgnoreCase))
        {
            break;
        }

        var request = new Request();
        if (firstTurn)
        {
            request.AddItem(MessageItem.System("You are a helpful assistant with access to tools. " +
                "Use them when needed to answer questions accurately."));
            firstTurn = false;
        }
        request.AddItem(MessageItem.User(userInput));
        const int maxToolRounds = 10;
        for (var round = 0; ; round++)
        {
            using (var currentRequest = request)
            using (var response = await session.ProcessRequestAsync(currentRequest, ct))
            {
                var calls = response.OfType<ToolCallItem>().ToArray();
                if (calls.Length == 0)
                {
                    Console.WriteLine($"Assistant: {Utils.GetResponseText(response)}\n");
                    break;
                }
                if (round >= maxToolRounds)
                {
                    throw new InvalidOperationException("The model exceeded the tool-call round limit.");
                }
                var followUp = new Request();
                try
                {
                    foreach (var call in calls)
                    {
                        using var arguments = JsonDocument.Parse(call.Arguments);
                        Console.WriteLine($"  Tool call: {call.Name}({arguments.RootElement})");
                        followUp.AddItem(new ToolResultItem(call.CallId, ExecuteTool(call.Name, arguments.RootElement)));
                    }
                }
                catch
                {
                    followUp.Dispose();
                    throw;
                }
                request = followUp;
            }
        }
    }
}
finally
{
    await model.UnloadAsync();
}
Console.WriteLine("Model unloaded. Goodbye!");
// </tool_loop>
// </complete_code>
