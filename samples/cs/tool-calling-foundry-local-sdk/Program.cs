// <complete_code>
// <imports>
using Microsoft.AI.Foundry.Local;
using System.Globalization;
using System.Text.Json;
// </imports>

// <init>
await FoundryLocalManager.CreateAsync(new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
}, Utils.GetAppLogger());
using var mgr = FoundryLocalManager.Instance;
await mgr.DownloadAndRegisterEpsAsync();
// </init>

// <model_setup>
var catalog = await mgr.GetCatalogAsync();
var model = await catalog.GetModelAsync("qwen2.5-0.5b") ?? throw new Exception("Model not found");
await model.DownloadAsync(progress => Console.Write($"\rDownloading model: {progress:F2}%"));
Console.WriteLine();
await model.LoadAsync();
// </model_setup>

try
{
    using var session = new ChatSession(model);
    session.SetStreaming(true);
    session.SetOptions(new RequestOptions { ToolChoice = ToolChoice.Required });

    // <tool_definitions>
    session.AddToolDefinition("multiply_numbers", "A tool for multiplying two numbers.", """
        {
          "type": "object",
          "properties": {
            "first": { "type": "integer", "description": "The first number in the operation" },
            "second": { "type": "integer", "description": "The second number in the operation" }
          },
          "required": ["first", "second"]
        }
        """);
    // </tool_definitions>

    // <tool_loop>
    using var request = new Request();
    request.AddItem(MessageItem.System("You are a helpful assistant. Use the provided tools to answer the question."));
    request.AddItem(MessageItem.User("What is the answer to 7 multiplied by 6?"));
    using var followUp = new Request();
    var callCount = 0;
    await using (var stream = session.ProcessStreamingRequestAsync(request))
    {
        await foreach (var item in stream)
        {
            using (item)
            {
                if (item is ToolCallItem call)
                {
                    if (call.Name != "multiply_numbers")
                    {
                        throw new InvalidOperationException($"Unexpected tool: {call.Name}");
                    }
                    using var arguments = JsonDocument.Parse(call.Arguments);
                    var first = arguments.RootElement.GetProperty("first").GetInt32();
                    var second = arguments.RootElement.GetProperty("second").GetInt32();
                    var result = Utils.MultiplyNumbers(first, second);
                    Console.WriteLine($"Invoking {call.Name}({first}, {second}): {result}");
                    followUp.AddItem(new ToolResultItem(call.CallId, result.ToString(CultureInfo.InvariantCulture)));
                    callCount++;
                }
                else if (item is TextItem { Type: TextItemType.Default } text)
                {
                    Console.Write(text.Text);
                }
            }
        }
    }
    if (callCount == 0)
    {
        throw new InvalidOperationException("The model did not produce the required tool call.");
    }

    // The session retains the assistant's tool calls; submit only their results.
    session.SetOptions(new RequestOptions { ToolChoice = ToolChoice.None });
    await using (var stream = session.ProcessStreamingRequestAsync(followUp))
    {
        await foreach (var item in stream)
        {
            using (item)
            {
                if (item is TextItem { Type: TextItemType.Default } text)
                {
                    Console.Write(text.Text);
                    Console.Out.Flush();
                }
            }
        }
    }
    Console.WriteLine();
    // </tool_loop>
}
finally
{
    // <cleanup>
    await model.UnloadAsync();
    // </cleanup>
}
// </complete_code>
