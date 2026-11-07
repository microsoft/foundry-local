// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Text.Json;

internal static class NonGenerativeValidation
{
    internal static void ValidateJson(JsonElement? value, string name)
    {
        if (value is JsonElement element)
        {
            ValidateJson(element, name);
        }
    }

    internal static void ValidateJson(JsonElement value, string name)
    {
        switch (value.ValueKind)
        {
            case JsonValueKind.Object:
                using (var properties = value.EnumerateObject())
                {
                    while (properties.MoveNext())
                    {
                        var property = properties.Current;
                        ValidateJson(property.Value, $"{name}.{property.Name}");
                    }
                }
                break;
            case JsonValueKind.Array:
                var index = 0;
                using (var items = value.EnumerateArray())
                {
                    while (items.MoveNext())
                    {
                        ValidateJson(items.Current, $"{name}[{index++}]");
                    }
                }
                break;
            case JsonValueKind.Number:
                if (!value.TryGetInt64(out _) &&
                    (!value.TryGetDouble(out var number) || !IsFinite(number)))
                {
                    throw new ArgumentException($"{name} numbers must be finite and JSON-compatible.");
                }
                break;
            case JsonValueKind.Undefined:
                throw new ArgumentException($"{name} must be a valid JSON value.");
        }
    }

    internal static int GetArrayOrObjectLength(this JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Array)
        {
            return value.GetArrayLength();
        }

        var count = 0;
        using var properties = value.EnumerateObject();
        while (properties.MoveNext())
        {
            count++;
        }
        return count;
    }

    internal static bool IsFinite(float value) =>
        !float.IsNaN(value) && !float.IsInfinity(value);

    internal static bool IsFinite(double value) =>
        !double.IsNaN(value) && !double.IsInfinity(value);
}
