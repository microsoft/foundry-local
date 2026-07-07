// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

using System;
using System.Runtime.CompilerServices;

/// <summary>
/// Cross-TFM throw helpers. <c>ArgumentNullException.ThrowIfNull(object?, string?)</c>
/// is .NET 6+ and <c>ObjectDisposedException.ThrowIf(bool, object)</c> is .NET 7+;
/// neither exists on netstandard2.0, so we forward where available and fall back otherwise.
/// </summary>
internal static class Throw
{
    public static void IfNull(object? value, [CallerArgumentExpression(nameof(value))] string? paramName = null)
    {
#if NET6_0_OR_GREATER
        ArgumentNullException.ThrowIfNull(value, paramName);
#else
        if (value is null)
        {
            throw new ArgumentNullException(paramName);
        }
#endif
    }

    public static void IfDisposed(bool condition, object instance)
    {
#if NET7_0_OR_GREATER
        ObjectDisposedException.ThrowIf(condition, instance);
#else
        if (condition)
        {
            throw new ObjectDisposedException(instance?.GetType().FullName);
        }
#endif
    }
}
