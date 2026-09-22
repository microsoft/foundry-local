// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

/// <summary>
/// Internal — well-known parameter key strings for the native KVP wire format. Mirrors the
/// <c>FOUNDRY_LOCAL_PARAM_*</c> macros in <c>foundry_local_c.h</c>. Public callers should use the typed
/// <see cref="RequestOptions"/> / <see cref="SearchOptions"/> API instead; <see cref="RequestOptions.AdditionalOptions"/>
/// is the escape hatch for params not yet typed (and therefore not represented here).
/// </summary>
internal static class SessionParam
{
    /// <summary>Sampling temperature. Float [0.0, 2.0]. Default is model-specific.</summary>
    public const string Temperature = "temperature";

    /// <summary>Nucleus sampling. Float [0.0, 1.0].</summary>
    public const string TopP = "top_p";

    /// <summary>Top-k sampling. Int.</summary>
    public const string TopK = "top_k";

    /// <summary>Maximum tokens to generate. Int.</summary>
    public const string MaxOutputTokens = "max_output_tokens";

    /// <summary>Frequency penalty. Currently only the neutral value 0 is supported.</summary>
    public const string FrequencyPenalty = "frequency_penalty";

    /// <summary>Presence penalty. Currently only the neutral value 0 is supported.</summary>
    public const string PresencePenalty = "presence_penalty";

    /// <summary>Random seed for reproducible outputs. Int.</summary>
    public const string Seed = "seed";

    /// <summary>Legacy beam-search policy; unsupported by Engine backends. Bool ("true"/"false").</summary>
    public const string EarlyStopping = "early_stopping";

    /// <summary>Whether to sample (false = greedy). Bool ("true"/"false").</summary>
    public const string DoSample = "do_sample";

    /// <summary>Tool choice mode. String: "auto", "none", or "required".</summary>
    public const string ToolChoice = "tool_choice";
}
