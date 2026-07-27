// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

// Modern .NET (net7.0+) DllLoader bits: registers a NativeLibrary resolver and uses
// NativeLibrary.TryLoad for absolute-path loads.

#if NET7_0_OR_GREATER

namespace Microsoft.AI.Foundry.Local.Detail;

using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;

internal static partial class DllLoader
{
    static partial void PlatformInitialize()
    {
        NativeLibrary.SetDllImportResolver(typeof(NativeMethods).Assembly, ResolveDll);
    }

    private static partial bool TryLoadNativeLibrary(string path, out IntPtr handle)
    {
        return NativeLibrary.TryLoad(path, out handle);
    }
}

#endif
