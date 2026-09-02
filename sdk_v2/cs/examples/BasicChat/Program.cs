// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace BasicChat;

using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;

internal static class Program
{
    private static readonly string[] s_preferredModels = ["qwen2.5-0.5b", "qwen3.5-0.8b"];

    public static async Task Main()
    {
        var modelCacheDir = Environment.GetEnvironmentVariable("FOUNDRY_LOCAL_SAMPLE_CACHE_DIR");
        var configuration = new Configuration
        {
            AppName = "FoundryLocalBasicChat",
            ModelCacheDir = string.IsNullOrWhiteSpace(modelCacheDir) ? null : modelCacheDir,
        };

        await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance).ConfigureAwait(false);
        using var manager = FoundryLocalManager.Instance;

        var catalog = await manager.GetCatalogAsync().ConfigureAwait(false);
        var cachedModels = await catalog.GetCachedModelsAsync().ConfigureAwait(false);
        var model = cachedModels
            .Where(IsCpuChatModel)
            .OrderBy(model => IsPreferredModel(model) ? 0 : 1)
            .ThenBy(model => model.Info.FileSizeMb ?? int.MaxValue)
            .ThenBy(model => model.Id, StringComparer.Ordinal)
            .FirstOrDefault();

        if (model is null)
        {
            foreach (var alias in s_preferredModels)
            {
                var candidate = await catalog.GetModelAsync(alias).ConfigureAwait(false);
                var cpuVariant = candidate?.Variants
                    .Where(IsCpuChatModel)
                    .OrderBy(variant => variant.Info.FileSizeMb ?? int.MaxValue)
                    .FirstOrDefault();
                if (candidate is not null && cpuVariant is not null)
                {
                    candidate.SelectVariant(cpuVariant);
                    model = candidate;
                    break;
                }
            }
            if (model is null)
            {
                throw new InvalidOperationException("No supported CPU chat model is available.");
            }
        }

        Console.WriteLine($"Using model: {model.Id}");
        if (!await model.IsCachedAsync().ConfigureAwait(false))
        {
            Console.WriteLine("Downloading model...");
            await model.DownloadAsync(progress => Console.Write($"\r  {progress:F1}%")).ConfigureAwait(false);
            Console.WriteLine();
        }
        await model.LoadAsync().ConfigureAwait(false);

        try
        {
            using var session = new ChatSession(model);
            using var request = new Request();
            request.AddItem(MessageItem.User(
                "What is the capital of France? Answer with only the city name."));

            using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);
            var responseText = string.Join(
                Environment.NewLine,
                response
                    .OfType<MessageItem>()
                    .Where(message => message.IsSimpleText())
                    .Select(message => message.GetSimpleText()));

            Console.WriteLine($"Assistant: {responseText}");

            if (string.IsNullOrWhiteSpace(responseText))
            {
                throw new InvalidOperationException("The model returned an empty response.");
            }

            if (!responseText.Contains("Paris", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(
                    $"Expected the response to identify Paris, but received: {responseText}");
            }
        }
        finally
        {
            await model.UnloadAsync().ConfigureAwait(false);
        }
    }

    private static bool IsCpuChatModel(IModel model)
    {
        var info = model.Info;
        var isChat = string.Equals(info.Task, "chat-completion", StringComparison.OrdinalIgnoreCase)
            || string.Equals(info.Task, "vision-language-chat", StringComparison.OrdinalIgnoreCase);
        var isCpu = info.Runtime?.DeviceType == DeviceType.CPU
            || string.Equals(
                info.Runtime?.ExecutionProvider,
                "CPUExecutionProvider",
                StringComparison.OrdinalIgnoreCase);

        return isChat && isCpu;
    }

    private static bool IsPreferredModel(IModel model)
    {
        return s_preferredModels.Any(preferred =>
            string.Equals(model.Alias, preferred, StringComparison.OrdinalIgnoreCase)
            || string.Equals(model.Info.Name, preferred, StringComparison.OrdinalIgnoreCase)
            || model.Id.StartsWith(preferred, StringComparison.OrdinalIgnoreCase));
    }
}
