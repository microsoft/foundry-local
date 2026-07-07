// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

public sealed class InputOutputInfo
{
    public IReadOnlyList<Item> Inputs { get; }
    public IReadOnlyList<Item> Outputs { get; }

    internal InputOutputInfo(List<Item> inputs, List<Item> outputs)
    {
        Inputs = inputs;
        Outputs = outputs;
    }
}
