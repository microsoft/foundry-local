// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Text.Json;
using System.Text.Json.Serialization.Metadata;

using Microsoft.AI.Foundry.Local.Detail;

/// <summary>
/// Base class for typed non-generative sessions that exchange one OpenAI-compatible JSON item.
/// </summary>
public abstract class NonGenerativeSession : Session
{
    private readonly Func<string, CancellationToken, Task<string>>? _testTransport;

    protected NonGenerativeSession(IModel model) : base(model)
    {
    }

    private protected NonGenerativeSession(
        Func<string, CancellationToken, Task<string>> testTransport) : base()
    {
        _testTransport = testTransport;
    }

    protected async Task<TResult> ProcessAsync<TRequest, TResult>(
        TRequest input,
        JsonTypeInfo<TRequest> requestTypeInfo,
        JsonTypeInfo<TResult> resultTypeInfo,
        CancellationToken ct)
    {
        Detail.Throw.IfNull(input);
        var requestJson = JsonSerializer.Serialize(input, requestTypeInfo);
        var responseJson = _testTransport == null
            ? await ProcessNativeJsonAsync(requestJson, ct).ConfigureAwait(false)
            : await _testTransport(requestJson, ct).ConfigureAwait(false);

        return JsonSerializer.Deserialize(responseJson, resultTypeInfo)
            ?? throw new JsonException($"Native response could not be deserialized as {typeof(TResult).Name}.");
    }

    private async Task<string> ProcessNativeJsonAsync(string requestJson, CancellationToken ct)
    {
#pragma warning disable CA2000 // Request takes ownership of the item.
        using var request = new Request();
        request.AddItem(TextItem.OpenAIJson(requestJson));
#pragma warning restore CA2000

        using var response = await ProcessRequestAsync(request, ct).ConfigureAwait(false);
        if (response.ItemCount != 1)
        {
            throw new FoundryLocalException(
                $"Expected one OpenAIJson TextItem from native response, got {response.ItemCount} items.");
        }

        using var item = response.GetItem(0);
        if (item is not TextItem textItem || textItem.Type != TextItemType.OpenAIJson)
        {
            throw new FoundryLocalException(
                $"Expected one OpenAIJson TextItem from native response, got {item.GetType().Name}.");
        }

        return textItem.Text;
    }
}
