// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;

using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
using Betalgo.Ranul.OpenAI.ObjectModels.ResponseModels;
using Betalgo.Ranul.OpenAI.ObjectModels.SharedModels;

using Microsoft.AI.Foundry.Local.OpenAI;

// Disambiguate from Microsoft.AI.Foundry.Local.ToolChoice (typed RequestOptions enum)
// which lives in the parent namespace and would otherwise shadow the Betalgo type here.
using OpenAIToolChoice = Betalgo.Ranul.OpenAI.ObjectModels.RequestModels.ToolChoice;

[JsonSerializable(typeof(ModelInfo))]
[JsonSerializable(typeof(List<ModelInfo>))]
[JsonSerializable(typeof(ChatCompletionCreateRequestExtended))]
[JsonSerializable(typeof(ChatCompletionCreateResponse))]
[JsonSerializable(typeof(AudioCreateTranscriptionRequest))]
[JsonSerializable(typeof(AudioCreateTranscriptionResponse))]
[JsonSerializable(typeof(EmbeddingCreateRequestExtended))]
[JsonSerializable(typeof(EmbeddingCreateResponse))]
[JsonSerializable(typeof(string[]))]
[JsonSerializable(typeof(EpInfo[]))]
[JsonSerializable(typeof(EpDownloadResult))]
[JsonSerializable(typeof(JsonElement))]
[JsonSerializable(typeof(ResponseFormatExtended))]
[JsonSerializable(typeof(OpenAIToolChoice))]
[JsonSerializable(typeof(ToolDefinition))]
[JsonSerializable(typeof(IList<ToolDefinition>))]
[JsonSerializable(typeof(FunctionDefinition))]
[JsonSerializable(typeof(IList<FunctionDefinition>))]
[JsonSerializable(typeof(PropertyDefinition))]
[JsonSerializable(typeof(IList<PropertyDefinition>))]
[JsonSerializable(typeof(LiveAudioTranscriptionRaw))]
[JsonSerializable(typeof(CoreErrorResponse))]
[JsonSourceGenerationOptions(DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
                             WriteIndented = false)]
internal partial class JsonSerializationContext : JsonSerializerContext
{
}
