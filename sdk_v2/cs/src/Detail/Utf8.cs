// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

using System;
using System.Runtime.InteropServices;
using System.Text;

/// <summary>
/// UTF-8 string marshalling helpers. <c>Marshal.PtrToStringUTF8(IntPtr)</c> is only
/// available on .NET Core / modern .NET, so we provide a netstandard2.0 fallback that walks
/// the buffer to the null terminator and decodes via <see cref="Encoding.UTF8"/>.
/// </summary>
internal static class Utf8
{
    /// <summary>
    /// Marshals a null-terminated UTF-8 string at <paramref name="ptr"/> into a managed string.
    /// Returns null if <paramref name="ptr"/> is <see cref="IntPtr.Zero"/>.
    /// </summary>
    public static string? PtrToString(IntPtr ptr)
    {
        if (ptr == IntPtr.Zero)
        {
            return null;
        }

#if NETSTANDARD2_0
        // Find null terminator.
        var length = 0;
        unsafe
        {
            var p = (byte*)ptr;
            while (p[length] != 0)
            {
                length++;
            }
        }

        if (length == 0)
        {
            return string.Empty;
        }

        var bytes = new byte[length];
        Marshal.Copy(ptr, bytes, 0, length);
        return Encoding.UTF8.GetString(bytes);
#else
        return Marshal.PtrToStringUTF8(ptr);
#endif
    }

    /// <summary>
    /// Allocates a CoTaskMem buffer containing the UTF-8 encoding of <paramref name="value"/>
    /// followed by a NUL terminator. Returns <see cref="IntPtr.Zero"/> for a null input.
    /// Caller must free with <see cref="Marshal.FreeCoTaskMem(IntPtr)"/>.
    ///
    /// <c>Marshal.StringToCoTaskMemUTF8(string?)</c> doesn't exist on netstandard2.0,
    /// so we encode + copy + NUL-terminate manually there.
    /// </summary>
    public static IntPtr StringToCoTaskMem(string? value)
    {
        if (value == null)
        {
            return IntPtr.Zero;
        }

#if NETSTANDARD2_0
        var bytes = Encoding.UTF8.GetBytes(value);
        var ptr = Marshal.AllocCoTaskMem(bytes.Length + 1);
        Marshal.Copy(bytes, 0, ptr, bytes.Length);
        Marshal.WriteByte(ptr, bytes.Length, 0);
        return ptr;
#else
        return Marshal.StringToCoTaskMemUTF8(value);
#endif
    }
}
