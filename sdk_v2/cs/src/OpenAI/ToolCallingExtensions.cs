// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.OpenAI;

using System.Text.Json.Serialization;

using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;

// Extend response format beyond the OpenAI spec for LARK grammars.
// Note: this type is registered in JsonSerializationContext for source-generated (AOT-safe)
// serialization. It currently has no derived types; if any are introduced, add
// [JsonPolymorphic] + [JsonDerivedType(...)] here so source-gen handles the discriminator.
public class ResponseFormatExtended : ResponseFormat
{
    // Ex:
    // 1. {"type": "text"}
    // 2. {"type": "json_object"}
    // 3. {"type": "json_schema", "json_schema": <JSON schema to use>}
    // 4. {"type": "lark_grammar", "lark_grammar": <LARK grammar to use>}
    [JsonPropertyName("lark_grammar")]
    public string? LarkGrammar { get; set; }
}
