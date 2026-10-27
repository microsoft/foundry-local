// <complete_code>
// <imports>
using System.Text.Json;
using Microsoft.AI.Foundry.Local;
using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
using Betalgo.Ranul.OpenAI.ObjectModels.ResponseModels;
using Betalgo.Ranul.OpenAI.ObjectModels.SharedModels;
using ToolChoice = Betalgo.Ranul.OpenAI.ObjectModels.RequestModels.ToolChoice;
using Microsoft.Extensions.Logging;
// </imports>

CancellationToken ct = CancellationToken.None;

// <tool_definitions>
// --- Tool definitions ---
List<ToolDefinition> tools =
[
    new ToolDefinition
    {
        Type = "function",
        Function = new FunctionDefinition()
        {
            Name = "get_weather",
            Description = "Get the current weather for a location",
            Parameters = new PropertyDefinition()
            {
                Type = "object",
                Properties = new Dictionary<string, PropertyDefinition>()
                {
                    { "location", new PropertyDefinition() { Type = "string", Description = "The city or location" } },
                    { "unit", new PropertyDefinition() { Type = "string", Description = "Temperature unit (celsius or fahrenheit)" } }
                },
                Required = ["location"]
            }
        }
    },
    new ToolDefinition
    {
        Type = "function",
        Function = new FunctionDefinition()
        {
            Name = "calculate",
            Description = "Perform a math calculation",
            Parameters = new PropertyDefinition()
            {
                Type = "object",
                Properties = new Dictionary<string, PropertyDefinition>()
                {
                    { "expression", new PropertyDefinition() { Type = "string", Description = "The math expression to evaluate" } }
                },
                Required = ["expression"]
            }
        }
    }
];

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

var chatClient = await model.GetChatClientAsync();
chatClient.Settings.ToolChoice = ToolChoice.Auto;

var messages = new List<ChatMessage>
{
    new ChatMessage
    {
        Role = "system",
        Content = "You are a helpful assistant with access to tools. " +
                  "Use them when needed to answer questions accurately."
    }
};
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

    messages.Add(new ChatMessage
    {
        Role = "user",
        Content = userInput
    });

    var response = await chatClient.CompleteChatAsync(
        messages, tools, ct
    );

    var choice = response.Choices[0].Message;

    if (choice.ToolCalls is { Count: > 0 })
    {
        messages.Add(choice);

        foreach (var toolCall in choice.ToolCalls)
        {
            var toolArgs = JsonDocument.Parse(
                toolCall.FunctionCall.Arguments
            ).RootElement;
            Console.WriteLine(
                $"  Tool call: {toolCall.FunctionCall.Name}({toolArgs})"
            );

            var result = ExecuteTool(
                toolCall.FunctionCall.Name, toolArgs
            );
            messages.Add(new ChatMessage
            {
                Role = "tool",
                ToolCallId = toolCall.Id,
                Content = result
            });
        }

        var finalResponse = await chatClient.CompleteChatAsync(
            messages, tools, ct
        );
        var answer = finalResponse.Choices[0].Message.Content ?? "";
        messages.Add(new ChatMessage
        {
            Role = "assistant",
            Content = answer
        });
        Console.WriteLine($"Assistant: {answer}\n");
    }
    else
    {
        var answer = choice.Content ?? "";
        messages.Add(new ChatMessage
        {
            Role = "assistant",
            Content = answer
        });
        Console.WriteLine($"Assistant: {answer}\n");
    }
}

await model.UnloadAsync();
Console.WriteLine("Model unloaded. Goodbye!");
// </tool_loop>
// </complete_code>
