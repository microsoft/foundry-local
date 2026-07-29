// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

// Legacy DllLoader bits for netstandard2.0 (.NET Framework 4.6.2+, Windows only).
//
// .NET Framework has no NativeLibrary.SetDllImportResolver, so we eagerly walk the same
// probe paths the modern resolver uses and pre-load foundry_local.dll via LoadLibraryW.
// Once it's resident in the process module table, the existing [DllImport] declarations
// in NativeMethods resolve by name with no further filesystem search.

#if !NET7_0_OR_GREATER

namespace Microsoft.AI.Foundry.Local.Detail;

using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;

internal static partial class DllLoader
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibraryW(string path);

    static partial void PlatformInitialize()
    {
        if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            throw new PlatformNotSupportedException(
                "The netstandard2.0 build of Microsoft.AI.Foundry.Local is only supported on " +
                ".NET Framework 4.6.2+ (Windows). Use the net8.0 or net9.0 build for cross-platform support.");
        }

        // Eagerly probe the same paths the modern resolver checks. ResolveDll returns
        // IntPtr.Zero if nothing was found; in that case we silently fall through and
        // let the OS loader try its own search when the first [DllImport] fires.
        ResolveDll(NativeMethods.LibraryName, typeof(NativeMethods).Assembly, null);
    }

    private static partial bool TryLoadNativeLibrary(string path, out IntPtr handle)
    {
        handle = LoadLibraryW(path);
        return handle != IntPtr.Zero;
    }
}

#endif
