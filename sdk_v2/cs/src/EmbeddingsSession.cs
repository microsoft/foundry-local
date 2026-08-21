// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

/// <summary>
/// An embeddings session for text-embedding models.
/// Validates the model task at construction time.
/// Stateless and concurrent — multiple requests can be processed in parallel against
/// the same loaded model.
/// </summary>
public sealed class EmbeddingsSession : Session
{
    /// <summary>
    /// Create an embeddings session from a loaded embeddings model.
    /// An embeddings session accepts <see cref="TextItem"/> inputs and produces
    /// one <see cref="TensorItem"/> per input containing the embedding vector.
    /// </summary>
    /// <param name="model">A loaded model whose task is "embeddings".</param>
    /// <exception cref="ArgumentException">If the model's task is not embeddings.</exception>
    public EmbeddingsSession(IModel model) : base(ValidateTask(model))
    {
    }

    // Validate the model's task BEFORE the base Session constructor runs. The base
    // constructor calls into native to create the session; checking first surfaces a
    // wrong-task model as an ArgumentException rather than a native error, and keeps
    // validation consistent across the typed sessions.
    private static IModel ValidateTask(IModel model)
    {
        Detail.Throw.IfNull(model);
        if (model.Info.Task != "embeddings")
        {
            throw new ArgumentException(
                $"EmbeddingsSession requires a model with task 'embeddings', but got '{model.Info.Task}'.",
                nameof(model));
        }

        return model;
    }
}
