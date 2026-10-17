// <complete_code>
using Microsoft.AI.Foundry.Local;
using OpenAI;
using OpenAI.Chat;
using System.ClientModel;
using System.Globalization;
using System.Text;
using System.Text.Json;

var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information,
    Web = new Configuration.WebService
    {
        Urls = "http://127.0.0.1:52495"
    }
};


// Initialize the singleton instance.
await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
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


// Get the model catalog
var catalog = await mgr.GetCatalogAsync();


// Get a model using an alias
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


// Start the web service
Console.Write($"Starting web service on {config.Web.Urls}...");
await mgr.StartWebServiceAsync();
Console.WriteLine("done.");


// <<<<<< OPEN AI SDK USAGE >>>>>>
// Use the OpenAI SDK to call the local Foundry web service

ApiKeyCredential key = new ApiKeyCredential("notneeded");
OpenAIClient client = new OpenAIClient(key, new OpenAIClientOptions
{
    Endpoint = new Uri(config.Web.Urls + "/v1"),
});


// Get chat client
var chatClient = client.GetChatClient(model.Id);


// Prepare messages
var messages = new List<ChatMessage>
{
    ChatMessage.CreateSystemMessage("You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question."),
    ChatMessage.CreateUserMessage("What is the answer to 7 multiplied by 6?")
};


// Prepare tools
var tools = new List<ChatTool>
{
    ChatTool.CreateFunctionTool(
        functionName: "multiply_numbers",
        functionDescription: "A tool for multiplying two numbers.",
        functionParameters: BinaryData.FromString("""
        {
            "type": "object",
            "properties": {
                "first": { "type": "integer", "description": "The first number in the operation" },
                "second": { "type": "integer", "description": "The second number in the operation" }
            },
            "required": ["first", "second"]
        }
        """)
    )
};


// Prepare chat completion options
var options = new ChatCompletionOptions
{
    ToolChoice = ChatToolChoice.CreateRequiredChoice()  // Force the model to make a tool call
};
foreach (var tool in tools)
{
    options.Tools.Add(tool);
}


// Get a streaming chat completion response
var completionUpdates = chatClient.CompleteChatStreaming(messages, options);
var toolUpdates = new SortedDictionary<int, (string? Id, string? Name, StringBuilder Arguments)>();
Console.Write($"[ASSISTANT]: ");
foreach (var completionUpdate in completionUpdates)
{
    if (completionUpdate.ContentUpdate.Count > 0)
    {
        Console.Write(completionUpdate.ContentUpdate[0].Text);
    }

    foreach (var update in completionUpdate.ToolCallUpdates)
    {
        if (!toolUpdates.TryGetValue(update.Index, out var call))
        {
            call = (null, null, new StringBuilder());
        }
        call.Id = update.ToolCallId ?? call.Id;
        call.Name = update.FunctionName ?? call.Name;
        if (update.FunctionArgumentsUpdate is not null)
        {
            call.Arguments.Append(update.FunctionArgumentsUpdate.ToString());
        }
        toolUpdates[update.Index] = call;
    }
}
Console.WriteLine();

var toolCalls = toolUpdates.Values.Select(call => ChatToolCall.CreateFunctionToolCall(
    call.Id ?? throw new InvalidOperationException("Tool call is missing its ID."),
    call.Name ?? throw new InvalidOperationException("Tool call is missing its function name."),
    BinaryData.FromString(call.Arguments.ToString()))).ToArray();
if (toolCalls.Length == 0)
{
    throw new InvalidOperationException("The model did not produce the required tool call.");
}
messages.Add(new AssistantChatMessage(toolCalls));

// Invoke tools called and append responses to the chat
foreach (var toolCall in toolCalls)
{
    if (toolCall.FunctionName != "multiply_numbers")
    {
        throw new InvalidOperationException($"Unexpected tool: {toolCall.FunctionName}");
    }
    using var arguments = JsonDocument.Parse(toolCall.FunctionArguments.ToString());
    var first = arguments.RootElement.GetProperty("first").GetInt32();
    var second = arguments.RootElement.GetProperty("second").GetInt32();

    Console.WriteLine($"\nInvoking tool: {toolCall.FunctionName} with arguments {first} and {second}");
    var result = Utils.MultiplyNumbers(first, second);
    Console.WriteLine($"Tool response: {result}");

    messages.Add(ChatMessage.CreateToolMessage(toolCall.Id, result.ToString(CultureInfo.InvariantCulture)));
}
Console.WriteLine("\nTool calls completed. Prompting model to continue conversation...\n");


// Prompt the model to continue the conversation after the tool call
messages.Add(ChatMessage.CreateSystemMessage("Respond only with the answer generated by the tool."));


// Set tool calling back to auto so that the model can decide whether to call
// the tool again or continue the conversation based on the new user prompt
options.ToolChoice = ChatToolChoice.CreateAutoChoice();


// Run the next turn of the conversation
Console.WriteLine("Chat completion response:");
completionUpdates = chatClient.CompleteChatStreaming(messages, options);
Console.Write($"[ASSISTANT]: ");
foreach (var completionUpdate in completionUpdates)
{
    if (completionUpdate.ContentUpdate.Count > 0)
    {
        Console.Write(completionUpdate.ContentUpdate[0].Text);
    }
}
Console.WriteLine();

// <<<<<< END OPEN AI SDK USAGE >>>>>>


// Tidy up
// Stop the web service and unload model
await mgr.StopWebServiceAsync();
await model.UnloadAsync();
// </complete_code>