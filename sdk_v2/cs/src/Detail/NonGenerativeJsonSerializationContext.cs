// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

using System.Text.Json.Serialization;

[JsonSerializable(typeof(RankingRequest))]
[JsonSerializable(typeof(RankingResult))]
[JsonSerializable(typeof(DecisionRequest))]
[JsonSerializable(typeof(DecisionResult))]
[JsonSourceGenerationOptions(
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    WriteIndented = false)]
internal partial class NonGenerativeJsonSerializationContext : JsonSerializerContext
{
}
