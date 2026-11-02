using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;

var configuration = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = LogLevel.Information,
};

await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance).ConfigureAwait(false);
using var manager = FoundryLocalManager.Instance;
await manager.DownloadAndRegisterEpsAsync().ConfigureAwait(false);

var catalog = await manager.GetCatalogAsync().ConfigureAwait(false);
var model = await catalog.GetModelAsync("qwen2.5-0.5b").ConfigureAwait(false)
    ?? throw new InvalidOperationException("The qwen2.5-0.5b model was not found.");
if (model.Info.SupportsToolCalling != true)
{
    throw new InvalidOperationException("The selected qwen2.5-0.5b variant does not support tool calling.");
}

Console.WriteLine($"Using tool-calling model: {model.Id}");

if (!await model.IsCachedAsync().ConfigureAwait(false))
{
    Console.WriteLine($"Downloading {model.Id}...");
    await model.DownloadAsync(
        progress => Console.Write($"\r  {progress:F1}%   "))
        .ConfigureAwait(false);
    Console.WriteLine();
}

await model.LoadAsync().ConfigureAwait(false);

try
{
    using var session = new ChatSession(model);
    session.AddToolDefinition(
        "get_weather",
        "Get the current weather for a city.",
        """
        {
          "type": "object",
          "properties": {
            "city": { "type": "string" }
          },
          "required": ["city"],
          "additionalProperties": false
        }
        """);

    using var firstRequest = new Request();
    firstRequest.AddItem(MessageItem.User("What is the weather in Paris?"));
    firstRequest.SetOptions(new RequestOptions
    {
        Search = new SearchOptions { Temperature = 0.0f, MaxOutputTokens = 256 },
        ToolChoice = ToolChoice.Required,
    });

    using var firstResponse = await session.ProcessRequestAsync(firstRequest).ConfigureAwait(false);
    var toolCall = GetToolCall(firstResponse);

    Console.WriteLine($"Tool call: {toolCall.Name}({toolCall.Arguments})");
    const string result = """{"temperature":22,"unit":"celsius","condition":"sunny"}""";

    using var secondRequest = new Request();
    secondRequest.AddItem(new ToolResultItem(toolCall.CallId, result));
    secondRequest.SetOptions(new RequestOptions
    {
        Search = new SearchOptions { Temperature = 0.0f, MaxOutputTokens = 256 },
        ToolChoice = ToolChoice.None,
    });

    using var secondResponse = await session.ProcessRequestAsync(secondRequest).ConfigureAwait(false);
    foreach (var item in secondResponse)
    {
        using (item)
        {
            if (item is MessageItem message)
            {
                Console.WriteLine($"Assistant: {message.GetSimpleText()}");
            }
        }
    }
}
finally
{
    await model.UnloadAsync().ConfigureAwait(false);
}

static (string CallId, string Name, string Arguments) GetToolCall(Response response)
{
    foreach (var item in response)
    {
        using (item)
        {
            if (item is ToolCallItem toolCall)
            {
                return (toolCall.CallId, toolCall.Name, toolCall.Arguments);
            }
        }
    }

    throw new InvalidOperationException("The model did not return a tool call.");
}
