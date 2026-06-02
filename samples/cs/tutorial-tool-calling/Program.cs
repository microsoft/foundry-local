// <complete_code>
// <imports>
using System.Text.Json;
using Microsoft.AI.Foundry.Local;
// </imports>

CancellationToken ct = CancellationToken.None;

// <tool_definitions>
// --- Tool implementations ---
// Each tool is invoked by name; arguments arrive as a parsed JSON object from the model
// and the result is returned as a JSON string sent back via ToolResultItem.
string ExecuteTool(string functionName, JsonElement arguments)
{
    switch (functionName)
    {
        case "get_weather":
            var location = arguments.GetProperty("location").GetString() ?? "unknown";
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
            var expression = arguments.GetProperty("expression").GetString() ?? "";
            try
            {
                var result = new System.Data.DataTable().Compute(expression, null);
                return JsonSerializer.Serialize(new { expression, result = result?.ToString() });
            }
            catch (Exception ex)
            {
                return JsonSerializer.Serialize(new { error = ex.Message });
            }

        default:
            return JsonSerializer.Serialize(new { error = $"Unknown function: {functionName}" });
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

await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
var mgr = FoundryLocalManager.Instance;

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

// Create a ChatSession and register the tool definitions once — they remain visible
// to the model for every request on this session. The session also retains the
// conversation history across turns.
using var session = new ChatSession(model);

session.AddToolDefinition(
    "get_weather",
    "Get the current weather for a location",
    /*lang=json,strict*/
    """
    {
      "type": "object",
      "properties": {
        "location": { "type": "string", "description": "The city or location" },
        "unit":     { "type": "string", "description": "Temperature unit (celsius or fahrenheit)" }
      },
      "required": ["location"]
    }
    """);

session.AddToolDefinition(
    "calculate",
    "Perform a math calculation",
    /*lang=json,strict*/
    """
    {
      "type": "object",
      "properties": {
        "expression": { "type": "string", "description": "The math expression to evaluate" }
      },
      "required": ["expression"]
    }
    """);

// Prime the session with a system message on the first turn (below).
bool firstTurn = true;
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

    // Build the user-turn request. Only new items are added — ChatSession holds history.
    using var request = new Request();
    if (firstTurn)
    {
        request.AddItem(MessageItem.System(
            "You are a helpful assistant with access to tools. " +
            "Use them when needed to answer questions accurately."));
        firstTurn = false;
    }

    request.AddItem(MessageItem.User(userInput));

    // Collect any tool calls the model emits during this turn.
    var pendingResults = new List<ToolResultItem>();
    string? directAnswer = null;

    using (var response = await session.ProcessRequestAsync(request, ct))
    {
        foreach (var item in response)
        {
            using (item)
            {
                if (item is ToolCallItem call)
                {
                    using var argsDoc = JsonDocument.Parse(call.Arguments);
                    var callArgs = argsDoc.RootElement;
                    Console.WriteLine($"  Tool call: {call.Name}({callArgs})");
                    var result = ExecuteTool(call.Name, callArgs);
                    Console.WriteLine($"  Tool result: {result}");
                    pendingResults.Add(new ToolResultItem(call.CallId, result));
                }
                else if (item is MessageItem msg)
                {
                    directAnswer = msg.GetSimpleText();
                }
            }
        }
    }

    if (pendingResults.Count > 0)
    {
        // Send tool results back so the model can incorporate them into its answer.
        using var followUp = new Request();
        foreach (var toolResult in pendingResults)
        {
            followUp.AddItem(toolResult);
        }

        string answer = "";
        using (var followUpResponse = await session.ProcessRequestAsync(followUp, ct))
        {
            using var item = followUpResponse.GetItem(0);
            if (item is MessageItem msg)
            {
                answer = msg.GetSimpleText();
            }
        }

        Console.WriteLine($"Assistant: {answer}\n");
    }
    else
    {
        Console.WriteLine($"Assistant: {directAnswer ?? ""}\n");
    }
}

session.Dispose();
await model.UnloadAsync();
mgr.Dispose();
Console.WriteLine("Model unloaded. Goodbye!");
// </tool_loop>
// </complete_code>
