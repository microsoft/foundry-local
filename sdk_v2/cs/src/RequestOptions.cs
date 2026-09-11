// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Collections.Generic;
using System.Globalization;

/// <summary>
/// Tool-choice mode for tool-enabled requests. Mirrors the C++ <c>flToolChoice</c> enum.
/// Serialized to the native layer as lower-case string values ("auto", "none", "required").
/// </summary>
public enum ToolChoice
{
    /// <summary>The model decides whether to call a tool.</summary>
    Auto,

    /// <summary>The model is prevented from calling any tool.</summary>
    None,

    /// <summary>The model is required to call a tool.</summary>
    Required,
}

/// <summary>
/// Pure sampling / decoder knobs. Each field is nullable — only non-null values
/// are forwarded to the C ABI. Mirrors the C++ <c>foundry_local::SearchOptions</c> struct.
/// </summary>
public sealed record SearchOptions
{
    /// <summary>Sampling temperature. Float [0.0, 2.0].</summary>
    public float? Temperature { get; init; }

    /// <summary>Nucleus sampling. Float [0.0, 1.0].</summary>
    public float? TopP { get; init; }

    /// <summary>Top-k sampling.</summary>
    public int? TopK { get; init; }

    /// <summary>Maximum tokens to generate.</summary>
    public int? MaxOutputTokens { get; init; }

    /// <summary>Frequency penalty. Currently only the neutral value 0 is supported.</summary>
    public float? FrequencyPenalty { get; init; }

    /// <summary>Presence penalty. Currently only the neutral value 0 is supported.</summary>
    public float? PresencePenalty { get; init; }

    /// <summary>Random seed for reproducibility.</summary>
    public int? Seed { get; init; }

    /// <summary>Stop on stop-sequence match.</summary>
    public bool? EarlyStopping { get; init; }

    /// <summary>Whether to sample (false = greedy).</summary>
    public bool? DoSample { get; init; }
}

/// <summary>
/// Options to apply to all requests (when passed to <see cref="Session.SetOptions(RequestOptions)"/>)
/// or to override session options for a single request (when passed to
/// <see cref="Request.SetOptions(RequestOptions)"/>).
/// Mirrors the C++ <c>foundry_local::RequestOptions</c> struct.
/// </summary>
public sealed record RequestOptions
{
    /// <summary>Sampling/decoder parameters.</summary>
    public SearchOptions Search { get; init; } = new();

    /// <summary>Tool-choice mode for tool-enabled requests.</summary>
    public ToolChoice? ToolChoice { get; init; }

    /// <summary>
    /// Passthrough for params not yet typed. Applied first; typed fields take precedence
    /// on key collision. <see cref="ToolChoice"/> is applied last and overrides any
    /// <c>tool_choice</c> key in this dictionary.
    /// </summary>
    public IDictionary<string, string> AdditionalOptions { get; init; } = new Dictionary<string, string>();

    /// <summary>
    /// Convert to the flat string-keyed dictionary that the marshalling layer hands to
    /// the native <c>flKeyValuePairs*</c>. Keys match the <c>FOUNDRY_LOCAL_PARAM_*</c>
    /// macros. Numeric formatting is invariant culture
    /// (dot decimal separator) — the native side parses with std::stof/std::stoi.
    /// </summary>
    internal Dictionary<string, string> ToDictionary()
    {
        // Precedence (matching C++): AdditionalOptions first, then typed Search fields,
        // then ToolChoice last.
        var result = new Dictionary<string, string>(AdditionalOptions);

        var s = Search;

        if (s.Temperature.HasValue)
        {
            result[SessionParam.Temperature] = s.Temperature.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (s.TopP.HasValue)
        {
            result[SessionParam.TopP] = s.TopP.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (s.TopK.HasValue)
        {
            result[SessionParam.TopK] = s.TopK.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (s.MaxOutputTokens.HasValue)
        {
            result[SessionParam.MaxOutputTokens] = s.MaxOutputTokens.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (s.FrequencyPenalty.HasValue)
        {
            result[SessionParam.FrequencyPenalty] = s.FrequencyPenalty.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (s.PresencePenalty.HasValue)
        {
            result[SessionParam.PresencePenalty] = s.PresencePenalty.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (s.Seed.HasValue)
        {
            result[SessionParam.Seed] = s.Seed.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (s.EarlyStopping.HasValue)
        {
            result[SessionParam.EarlyStopping] = s.EarlyStopping.Value ? "true" : "false";
        }

        if (s.DoSample.HasValue)
        {
            result[SessionParam.DoSample] = s.DoSample.Value ? "true" : "false";
        }

        if (ToolChoice.HasValue)
        {
            result[SessionParam.ToolChoice] = ToolChoiceToString(ToolChoice.Value);
        }

        return result;
    }

    private static string ToolChoiceToString(ToolChoice value)
    {
        return value switch
        {
            Microsoft.AI.Foundry.Local.ToolChoice.Auto => "auto",
            Microsoft.AI.Foundry.Local.ToolChoice.None => "none",
            Microsoft.AI.Foundry.Local.ToolChoice.Required => "required",
            _ => throw new System.ArgumentOutOfRangeException(nameof(value), value, "Unknown ToolChoice value."),
        };
    }
}
