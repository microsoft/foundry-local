// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System;
using System.Text.Json.Serialization;

using NativeModelType = Microsoft.AI.Foundry.Local.Detail.Native.Model;

[JsonConverter(typeof(JsonStringEnumConverter<DeviceType>))]
public enum DeviceType
{
    Invalid,
    CPU,
    GPU,
    NPU
}

[Obsolete("PromptTemplate is an internal model implementation detail and will be removed in a future release. Templates are applied automatically by ChatSession.", error: false)]
public record PromptTemplate
{
    [JsonPropertyName("system")]
    public string? System { get; init; }

    [JsonPropertyName("user")]
    public string? User { get; init; }

    [JsonPropertyName("assistant")]
    public string Assistant { get; init; } = default!;

    [JsonPropertyName("prompt")]
    public string Prompt { get; init; } = default!;
}

public record Runtime
{
    [JsonPropertyName("deviceType")]
    public DeviceType DeviceType { get; init; } = default!;

    // there are many different possible values; keep it open‑ended
    [JsonPropertyName("executionProvider")]
    public string ExecutionProvider { get; init; } = default!;
}

public record Parameter
{
    public required string Name { get; set; }
    public string? Value { get; set; }
}

public record ModelSettings
{
    [JsonPropertyName("parameters")]
    public Parameter[]? Parameters { get; set; }
}

public record ModelInfo
{
    [JsonPropertyName("id")]
    public required string Id { get; init; }

    [JsonPropertyName("name")]
    public required string Name { get; init; }

    [JsonPropertyName("version")]
    public int Version { get; init; }

    [JsonPropertyName("alias")]
    public required string Alias { get; init; }

    [JsonPropertyName("displayName")]
    public string? DisplayName { get; init; }

    [JsonPropertyName("providerType")]
    public required string ProviderType { get; init; }

    [JsonPropertyName("uri")]
    public required string Uri { get; init; }

    [JsonPropertyName("modelType")]
    public required string ModelType { get; init; }

    [JsonPropertyName("promptTemplate")]
#pragma warning disable CS0618 // PromptTemplate type is obsolete
    [Obsolete("PromptTemplate is an internal model implementation detail and will be removed in a future release. Templates are applied automatically by ChatSession.", error: false)]
    public PromptTemplate? PromptTemplate { get; init; }
#pragma warning restore CS0618

    [JsonPropertyName("publisher")]
    public string? Publisher { get; init; }

    [JsonPropertyName("modelSettings")]
    public ModelSettings? ModelSettings { get; init; }

    [JsonPropertyName("license")]
    public string? License { get; init; }

    [JsonPropertyName("licenseDescription")]
    public string? LicenseDescription { get; init; }

    [JsonPropertyName("cached")]
    public bool Cached { get; init; }


    [JsonPropertyName("task")]
    public string? Task { get; init; }

    [JsonPropertyName("runtime")]
    public Runtime? Runtime { get; init; }

    [JsonPropertyName("fileSizeMb")]
    public int? FileSizeMb { get; init; }

    [JsonPropertyName("supportsToolCalling")]
    public bool? SupportsToolCalling { get; init; }

    [JsonPropertyName("maxOutputTokens")]
    public long? MaxOutputTokens { get; init; }

    [JsonPropertyName("minFLVersion")]
    public string? MinFLVersion { get; init; }

    [JsonPropertyName("createdAt")]
    public long CreatedAtUnix { get; init; }

    [JsonPropertyName("contextLength")]
    public long? ContextLength { get; init; }

    [JsonPropertyName("inputModalities")]
    public string? InputModalities { get; init; }

    [JsonPropertyName("outputModalities")]
    public string? OutputModalities { get; init; }

    [JsonPropertyName("capabilities")]
    public string? Capabilities { get; init; }

    /// <summary>
    /// Create a ModelInfo record from a native Model's info properties.
    /// </summary>
    internal static ModelInfo FromNative(NativeModelType nativeModel)
    {
        var info = nativeModel.GetInfo();
        var deviceType = info.DeviceType switch
        {
            Detail.Interop.FlDeviceType.CPU => DeviceType.CPU,
            Detail.Interop.FlDeviceType.GPU => DeviceType.GPU,
            Detail.Interop.FlDeviceType.NPU => DeviceType.NPU,
            _ => DeviceType.Invalid,
        };

        var isCached = nativeModel.IsCached;
        var supportsToolCallingVal = info.SupportsToolCalling;
        bool? supportsToolCalling = supportsToolCallingVal >= 0 ? supportsToolCallingVal != 0 : null;
        var filesizeMb = info.FilesizeMb;
        var maxOutputTokens = info.MaxOutputTokens;
        var contextLength = info.GetIntProperty("context_length", -1);

        // PromptTemplate is intentionally not populated; it is deprecated and will be removed in a
        // future release. Templates are applied internally by ChatSession.
        var nativeSettings = info.GetModelSettings();
        ModelSettings? modelSettings = null;
        if (nativeSettings != null)
        {
            var parameters = new Parameter[nativeSettings.Count];
            int idx = 0;
            foreach (var kvp in nativeSettings)
            {
                parameters[idx++] = new Parameter { Name = kvp.Key, Value = kvp.Value };
            }
            modelSettings = new ModelSettings { Parameters = parameters };
        }

        return new ModelInfo
        {
            Id = info.Id,
            Name = info.Name,
            Version = info.Version,
            Alias = info.Alias,
            DisplayName = info.DisplayName,
            ProviderType = info.ModelProvider ?? string.Empty,
            Uri = info.Uri ?? string.Empty,
            ModelType = info.ModelType ?? string.Empty,
            Publisher = info.Publisher,
            License = info.License,
            LicenseDescription = info.LicenseDescription,
            Task = info.Task,
            Cached = isCached,
            ModelSettings = modelSettings,
            Runtime = new Runtime
            {
                DeviceType = deviceType,
                ExecutionProvider = info.ExecutionProvider ?? string.Empty,
            },
            FileSizeMb = ToNullableFileSizeMb(filesizeMb),
            SupportsToolCalling = supportsToolCalling,
            MaxOutputTokens = maxOutputTokens >= 0 ? maxOutputTokens : null,
            MinFLVersion = info.MinFlVersion,
            CreatedAtUnix = info.CreatedAtUnix,
            ContextLength = contextLength >= 0 ? contextLength : null,
            InputModalities = info.GetStringProperty("input_modalities"),
            OutputModalities = info.GetStringProperty("output_modalities"),
            Capabilities = info.GetStringProperty("capabilities"),
        };
    }

    internal static int? ToNullableFileSizeMb(long value) =>
        value is >= 0 and <= int.MaxValue ? (int)value : null;
}

/// <summary>
/// Mutable, caller-owned metadata used to register a local model.
/// </summary>
public sealed class ModelInfoBuilder
{
    internal Dictionary<string, string> StringProperties { get; } = new(StringComparer.Ordinal);
    internal Dictionary<string, long> IntProperties { get; } = new(StringComparer.Ordinal);

    public ModelInfoBuilder SetStringProperty(string key, string value)
    {
        ValidateKey(key);
        Detail.Throw.IfNull(value);
        Detail.Throw.IfContainsEmbeddedNul(value);
        StringProperties[key] = value;
        return this;
    }

    public ModelInfoBuilder SetIntProperty(string key, long value)
    {
        ValidateKey(key);
        IntProperties[key] = value;
        return this;
    }

    private static void ValidateKey(string key)
    {
        if (string.IsNullOrEmpty(key))
        {
            throw new ArgumentException("Property key cannot be null or empty.", nameof(key));
        }
        Detail.Throw.IfContainsEmbeddedNul(key);
    }
}

/// <summary>
/// Well-known property keys accepted by <see cref="ModelInfoBuilder.SetStringProperty"/> and
/// <see cref="ModelInfoBuilder.SetIntProperty"/> when constructing metadata for local model registration.
/// </summary>
public static class ModelInfoPropertyKeys
{
    public const string DisplayName = "display_name";
    public const string ModelType = "type";
    public const string Publisher = "publisher";
    public const string License = "license";
    public const string LicenseDescription = "license_description";
    public const string Task = "task";
    public const string ModelProvider = "model_provider";
    public const string MinimumFoundryLocalVersion = "min_fl_version";
    public const string ParentUri = "parent_uri";
    public const string ToolCallStart = "tool_call_start";
    public const string ToolCallEnd = "tool_call_end";
    public const string ReasoningStart = "reasoning_start";
    public const string ReasoningEnd = "reasoning_end";
    public const string DeviceType = "device_type";
    public const string ExecutionProvider = "execution_provider";
    public const string EntityType = "entity_type";
    public const string Author = "author";
    public const string Quantization = "quantization";
    public const string CreationTime = "creation_time";
    public const string InputModalities = "input_modalities";
    public const string OutputModalities = "output_modalities";
    public const string Capabilities = "capabilities";
    public const string SupportsToolCalling = "supports_tool_calling";
    public const string SupportsReasoning = "supports_reasoning";
    public const string FileSizeMb = "filesize_mb";
    public const string MaxOutputTokens = "max_output_tokens";
    public const string CreatedAtUnix = "created_at_unix";
    public const string IsTestModel = "is_test_model";
    public const string ContextLength = "context_length";
    public const string SupportsHybridReasoning = "supports_hybrid_reasoning";
}
