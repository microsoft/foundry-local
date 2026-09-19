// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

/// <summary>
/// A chat session for chat-completion models.
/// Validates the model task at construction time.
/// </summary>
public sealed class ChatSession : Session
{
    /// <summary>
    /// Create a chat session from a loaded chat-completion model.
    /// A chat session supports MessageItem input and produces MessageItem output.
    /// If used, the streaming callback will produce TextItem output (next token) or ToolCallItem output.
    ///
    /// The session will accumulate previous input and output MessageItem instances so only new input is required
    /// when creating a Request. <see cref="Request"/>
    ///
    /// Options and tool definitions that are set are applied to all requests in the session.
    /// A per-request option value will override the session option value.
    /// </summary>
    /// <param name="model">A loaded model whose task is "chat-completion" or "vision-language-chat".</param>
    /// <exception cref="ArgumentException">If the model's task is not a supported chat task.</exception>
    public ChatSession(IModel model) : base(ValidateTask(model))
    {
    }

    // Validate the model's task BEFORE the base Session constructor runs. The base
    // constructor calls into native to create the session; checking first surfaces a
    // wrong-task model as an ArgumentException rather than a native error, and keeps
    // validation consistent across the typed sessions.
    private static IModel ValidateTask(IModel model)
    {
        Detail.Throw.IfNull(model);
        if (model.Info.Task != "chat-completion" && model.Info.Task != "vision-language-chat")
        {
            throw new ArgumentException(
                $"ChatSession requires a model with task 'chat-completion' or 'vision-language-chat', but got '{model.Info.Task}'.",
                nameof(model));
        }

        return model;
    }

    /// <summary>
    /// Add a function tool definition so the model can request tool calls. The tool's arguments are
    /// a JSON object conforming to <paramref name="jsonSchema"/>, which is required and must be
    /// valid JSON. Names are case-sensitive and must be unique within the session across kinds.
    /// All three strings must not contain embedded NUL characters.
    /// </summary>
    /// <returns>This session (fluent).</returns>
    public ChatSession AddToolDefinition(string name, string description, string jsonSchema)
    {
        ThrowIfDisposed();
        ValidateNativeString(name, nameof(name));
        ValidateNativeString(description, nameof(description));
        ValidateNativeString(jsonSchema, nameof(jsonSchema));
        ExecuteNative(session => session.AddToolDefinition(name, description, jsonSchema));
        return this;
    }

    /// <summary>
    /// Add a custom tool definition: a tool whose arguments are a single free-form text payload
    /// rather than a JSON object. The schema the model is prompted with is synthesized natively, so
    /// no schema is supplied here, and the arguments of a generated call carry the raw text the
    /// model produced. The name and description must not contain embedded NUL characters.
    /// </summary>
    /// <returns>This session (fluent).</returns>
    public ChatSession AddCustomToolDefinition(string name, string description)
    {
        ThrowIfDisposed();
        ValidateNativeString(name, nameof(name));
        ValidateNativeString(description, nameof(description));
        ExecuteNative(session => session.AddToolDefinition(name, description, string.Empty, FlToolKind.Custom));
        return this;
    }

    /// <summary>
    /// Remove a previously-added tool definition by name. Useful when the available tool set
    /// changes mid-conversation. The name must not contain embedded NUL characters.
    /// </summary>
    /// <returns>True if a matching tool was found and removed; false if no tool with that name was registered.</returns>
    public bool RemoveToolDefinition(string toolName)
    {
        ThrowIfDisposed();
        ValidateNativeString(toolName, nameof(toolName));
        return ExecuteNative(session => session.RemoveToolDefinition(toolName));
    }

    private static void ValidateNativeString(string value, string paramName)
    {
        Detail.Throw.IfNull(value, paramName);
        foreach (var character in value)
        {
            if (character == '\0')
            {
                throw new ArgumentException("Value must not contain an embedded NUL character.", paramName);
            }
        }
    }

    /// <summary>
    /// Get the number of completed turns in the session.
    /// </summary>
    public ulong TurnCount
    {
        get
        {
            ThrowIfDisposed();
            return ExecuteNative(session => session.TurnCount);
        }
    }

    /// <summary>
    /// Undo the last <paramref name="count"/> turns: rewinds the generator and removes
    /// the turns' messages from history. If all turns are undone, the cached generator is destroyed.
    /// </summary>
    public void UndoTurns(ulong count)
    {
        ThrowIfDisposed();
        ExecuteNative(session => session.UndoTurns(count));
    }

    /// <summary>
    /// Capture this session's current state and <paramref name="request"/> synchronously, then
    /// execute an exact token-budget preflight asynchronously. The captured operation remains valid
    /// after the source session and request are disposed. The <see cref="FoundryLocalManager"/>
    /// must not be disposed until the returned task completes.
    /// </summary>
    public Task<RequestPreflightResult> PreflightRequestAsync(
        Request request,
        CancellationToken ct = default)
    {
        ThrowIfDisposed();
        Detail.Throw.IfNull(request);

        var nativeSession = GetNativeSession();
        var status = Api.Inference.SessionCreateRequestPreflight(
            nativeSession.Ptr, request.Ptr, out var preflightPtr);
        Api.CheckStatus(status);

        var operation = new RequestPreflightOperation(preflightPtr);
        return ExecutePreflightAsync(operation, ct);
    }

    private static async Task<RequestPreflightResult> ExecutePreflightAsync(
        RequestPreflightOperation operation,
        CancellationToken ct)
    {
#pragma warning disable IDISP007 // Ownership is transferred by PreflightRequestAsync to this async helper.
        using (operation)
#pragma warning restore IDISP007
        {
            return await Task.Run(operation.Execute, ct).ConfigureAwait(false);
        }
    }
}
