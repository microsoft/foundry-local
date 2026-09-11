// <complete_code>
// <imports>
using Microsoft.AI.Foundry.Local;
using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
using Betalgo.Ranul.OpenAI.ObjectModels.SharedModels;
using System.Text.Json;
// </imports>

// <init>
CancellationToken ct = new CancellationToken();

var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
};


// Initialize the singleton instance.
await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
var mgr = FoundryLocalManager.Instance;


// Ensure that any Execution Provider (EP) downloads run and are completed.
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
// </init>


// <model_setup>
// Get the model catalog
var catalog = await mgr.GetCatalogAsync();


// Get a model using an alias.
var model = await catalog.GetModelAsync("qwen2.5-0.5b") ?? throw new Exception("Model not found");


// Download the model (the method skips download if already cached)
await model.DownloadAsync(progress =>
{
    Console.Write($"\rDownloading model: {progress:F2}%");
    if (progress >= 100f)
    {
        Console.WriteLine();
    }
});


// Load the model
Console.Write($"Loading model {model.Id}...");
await model.LoadAsync();
Console.WriteLine("done.");
// </model_setup>


// Get a chat client
var chatClient = await model.GetChatClientAsync();
chatClient.Settings.ToolChoice = ToolChoice.Required; // Force the model to make a tool call


// Prepare messages
List<ChatMessage> messages =
[
    new ChatMessage { Role = "system", Content = "You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question." },
    new ChatMessage { Role = "user", Content = "What is the answer to 7 multiplied by 6?" }
];


// <tool_definitions>
// Prepare tools
List<ToolDefinition> tools =
[
    new ToolDefinition
    {
        Type = "function",
        Function = new FunctionDefinition()
        {
            Name = "multiply_numbers",
            Description = "A tool for multiplying two numbers.",
            Parameters = new PropertyDefinition()
            {
                Type = "object",
                Properties = new Dictionary<string, PropertyDefinition>()
                {
                    { "first", new PropertyDefinition() { Type = "integer", Description = "The first number in the operation" } },
                    { "second", new PropertyDefinition() { Type = "integer", Description = "The second number in the operation" } }
                },
                Required = ["first", "second"]
            }
        }
    }
];
// </tool_definitions>


// <tool_loop>
// Get a streaming chat completion response
var toolCalls = new List<ToolCall>();
Console.WriteLine("Chat completion response:");
var streamingResponse = chatClient.CompleteChatStreamingAsync(messages, tools, ct);
await foreach (var chunk in streamingResponse)
{
    var content = chunk.Choices[0].Message.Content;
    Console.Write(content);
    Console.Out.Flush();

    // Tool calls arrive in their own delta chunk, ahead of the final chunk that reports
    // finish_reason — that last chunk carries no tool call data of its own. Each call is
    // delivered complete in a single chunk, so collect them as they stream past.
    var chunkToolCalls = chunk.Choices[0].Message.ToolCalls;
    if (chunkToolCalls is { Count: > 0 })
    {
        toolCalls.AddRange(chunkToolCalls);
    }
}
Console.WriteLine();


// Invoke tools called and append responses to the chat
// All calls came from one assistant turn, so replay them together before adding their individual results.
if (toolCalls.Count > 0)
{
    messages.Add(new ChatMessage { Role = "assistant", ToolCalls = toolCalls });
}

foreach (var toolCall in toolCalls)
{
    var call = toolCall.FunctionCall;
    if (call?.Name == "multiply_numbers")
    {
        var arguments = JsonSerializer.Deserialize<Dictionary<string, int>>(call.Arguments!)!;
        var first = arguments["first"];
        var second = arguments["second"];

        Console.WriteLine($"\nInvoking tool: {call.Name} with arguments {first} and {second}");
        var result = Utils.MultiplyNumbers(first, second);
        Console.WriteLine($"Tool response: {result.ToString()}");

        var response = new ChatMessage
        {
            Role = "tool",
            ToolCallId = toolCall.Id,
            Content = result.ToString(),
        };
        messages.Add(response);
    }
}
Console.WriteLine("\nTool calls completed. Prompting model to continue conversation...\n");


// Prompt the model to continue the conversation after the tool call
messages.Add(new ChatMessage { Role = "system", Content = "Respond only with the answer generated by the tool." });


// Set tool calling back to auto so that the model can decide whether to call
// the tool again or continue the conversation based on the new user prompt
chatClient.Settings.ToolChoice = ToolChoice.Auto;


// Run the next turn of the conversation
Console.WriteLine("Chat completion response:");
streamingResponse = chatClient.CompleteChatStreamingAsync(messages, tools, ct);
await foreach (var chunk in streamingResponse)
{
    var content = chunk.Choices[0].Message.Content;
    Console.Write(content);
    Console.Out.Flush();
}
Console.WriteLine();
// </tool_loop>


// <cleanup>
// Tidy up - unload the model
await model.UnloadAsync();
// </cleanup>
// </complete_code>