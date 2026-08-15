// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

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
    public ChatSession(IModel model) : base(model)
    {
        if (model.Info.Task != "chat-completion" && model.Info.Task != "vision-language-chat")
        {
            throw new ArgumentException(
                $"ChatSession requires a model with task 'chat-completion' or 'vision-language-chat', but got '{model.Info.Task}'.",
                nameof(model));
        }
    }

    /// <summary>
    /// Add a tool definition so the model can request tool calls.
    /// </summary>
    /// <returns>This session (fluent).</returns>
    public ChatSession AddToolDefinition(string name, string description, string jsonSchema)
    {
        ThrowIfDisposed();
        GetNativeSession().AddToolDefinition(name, description, jsonSchema);
        return this;
    }

    /// <summary>
    /// Remove a previously-added tool definition by name. Useful when the available tool set
    /// changes mid-conversation.
    /// </summary>
    /// <returns>True if a matching tool was found and removed; false if no tool with that name was registered.</returns>
    public bool RemoveToolDefinition(string toolName)
    {
        ThrowIfDisposed();
        return GetNativeSession().RemoveToolDefinition(toolName);
    }

    /// <summary>
    /// Get the number of completed turns in the session.
    /// </summary>
    public ulong TurnCount
    {
        get
        {
            ThrowIfDisposed();
            return GetNativeSession().TurnCount;
        }
    }

    /// <summary>
    /// Undo the last <paramref name="count"/> turns: rewinds the generator and removes
    /// the turns' messages from history. If all turns are undone, the cached generator is destroyed.
    /// </summary>
    public void UndoTurns(ulong count)
    {
        ThrowIfDisposed();
        GetNativeSession().UndoTurns(count);
    }
}
