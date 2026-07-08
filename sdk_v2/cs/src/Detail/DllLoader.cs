// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;

/// <summary>
/// Handles native DLL resolution for foundry_local.dll.
///
/// We also preload onnxruntime and onnxruntime-genai by absolute path before
/// foundry_local loads. The loader binds foundry_local's import / NEEDED entries
/// for those libs by base name against the per-process loaded-module table, so
/// whichever copy is loaded first wins. Best-effort only: if another component
/// in the process (e.g. Windows App SDK) loaded ORT before we get here, the
/// loader has already pinned their copy and our preload is a no-op —
/// foundry_local will bind to theirs. ORT must be loaded before GenAI (GenAI
/// imports ORT).
///
/// Two TFM-conditional partials provide the platform-specific bits:
/// <list type="bullet">
/// <item><description><c>DllLoader.Modern.cs</c> (.NET 7+): registers a
/// <c>NativeLibrary</c> resolver and uses
/// <c>NativeLibrary.TryLoad(string, out IntPtr)</c> for absolute-path loads.</description></item>
/// <item><description><c>DllLoader.NetStandard.cs</c> (netstandard2.0 → .NET Framework on
/// Windows): eagerly walks the probe paths at <see cref="Initialize"/> time and pre-loads
/// the native DLL via <c>LoadLibraryW</c>; the existing <c>[DllImport]</c> calls then
/// resolve by name from the process module table.</description></item>
/// </list>
///
/// Must be initialized before any P/Invoke calls to the native library.
/// </summary>
internal static partial class DllLoader
{
    private static bool _initialized;
    private static readonly object _lock = new();

    // Hold ORT and GenAI handles for the process lifetime so the preloaded
    // copies stay resident and win base-name resolution for foundry_local's imports.
    private static IntPtr _ortHandle;
    private static IntPtr _genAiHandle;

    // Name of the redirect file written by the csproj when FoundryLocalNativeBinDir is set.
    // Contains a single line: the absolute path to the C++ build output directory.
    private const string RedirectFileName = "foundry_local.native.cfg";

    internal static void Initialize()
    {
        if (_initialized)
        {
            return;
        }

        lock (_lock)
        {
            if (_initialized)
            {
                return;
            }

            PlatformInitialize();
            _initialized = true;
        }
    }

    /// <summary>
    /// TFM-specific bootstrap. On modern .NET this registers the resolver callback;
    /// on netstandard2.0 it eagerly walks the probe paths and pre-loads the native DLL.
    /// </summary>
    static partial void PlatformInitialize();

    /// <summary>
    /// TFM-specific native library load. Modern uses
    /// <c>NativeLibrary.TryLoad(string, out IntPtr)</c>; netstandard2.0 uses
    /// <c>LoadLibraryW</c> on Windows.
    /// </summary>
    private static partial bool TryLoadNativeLibrary(string path, out IntPtr handle);

    private static string AddLibraryExtension(string name) =>
        RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? $"{name}.dll" :
        RuntimeInformation.IsOSPlatform(OSPlatform.Linux) ? $"lib{name}.so" :
        RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? $"lib{name}.dylib" :
        throw new PlatformNotSupportedException();

    /// <summary>
    /// Add a directory to the process PATH so the OS loader finds transitive native
    /// dependencies when foundry_local is loaded. This is additive and process-scoped —
    /// unlike SetDefaultDllDirectories, it does not restrict the existing search order.
    /// </summary>
    private static void AddToNativeSearchPath(string directory)
    {
        var currentPath = Environment.GetEnvironmentVariable("PATH") ?? "";
        var dirs = currentPath.Split(Path.PathSeparator);

        if (!dirs.Contains(directory, StringComparer.OrdinalIgnoreCase))
        {
            Environment.SetEnvironmentVariable("PATH", directory + Path.PathSeparator + currentPath);
        }
    }

    /// <summary>
    /// Try to load from a redirect file written by the build when FoundryLocalNativeBinDir is set.
    /// This avoids copying any native binaries for local dev — they're loaded directly from the
    /// C++ build output directory.
    /// </summary>
    private static IntPtr TryLoadFromRedirect(string libraryName)
    {
        var cfgPath = Path.Combine(AppContext.BaseDirectory, RedirectFileName);
        if (!File.Exists(cfgPath))
        {
            return IntPtr.Zero;
        }

        var nativeBinDir = File.ReadAllText(cfgPath).Trim();
        if (string.IsNullOrEmpty(nativeBinDir) || !Directory.Exists(nativeBinDir))
        {
            return IntPtr.Zero;
        }

        var libraryPath = Path.Combine(nativeBinDir, AddLibraryExtension(libraryName));
        if (!File.Exists(libraryPath))
        {
            return IntPtr.Zero;
        }

        // Prepend to PATH so the OS loader finds transitive deps (azure-core, etc.)
        AddToNativeSearchPath(nativeBinDir);
        PreloadOrtIfPresent(nativeBinDir);

        if (TryLoadNativeLibrary(libraryPath, out var handle))
        {
            LogResolution($"redirect file ({RedirectFileName})", libraryPath);
            return handle;
        }

        return IntPtr.Zero;
    }

    private static IntPtr ResolveDll(string libraryName, System.Reflection.Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != NativeMethods.LibraryName)
        {
            return IntPtr.Zero;
        }

        // 0. Local dev redirect — load directly from C++ build output (no copy).
        // The redirect file is written by the SDK csproj only when FoundryLocalNativeBinDir
        // is set (i.e. an explicit local-dev override). In CI / NuGet-consumed builds the
        // file should never exist; if one is found here it almost certainly indicates a
        // stale dev-build artifact and is worth flagging in the load log so the source of
        // any "wrong DLL loaded" surprise is obvious from the test output.
        var redirectResult = TryLoadFromRedirect(libraryName);
        if (redirectResult != IntPtr.Zero)
        {
            return redirectResult;
        }

        // 1. Check AppContext.BaseDirectory (co-located DLLs — flat publish / NuGet .targets copy)
        var libraryPath = Path.Combine(AppContext.BaseDirectory, AddLibraryExtension(libraryName));
        if (File.Exists(libraryPath))
        {
            AddToNativeSearchPath(AppContext.BaseDirectory);
            PreloadOrtIfPresent(AppContext.BaseDirectory);

            if (TryLoadNativeLibrary(libraryPath, out var handle))
            {
                LogResolution("BaseDirectory", libraryPath);
                return handle;
            }
        }

        // 2. Check runtimes/<rid>/native/ (NuGet layout)
        var os = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "win" :
                 RuntimeInformation.IsOSPlatform(OSPlatform.Linux) ? "linux" :
                 RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? "osx" :
                 throw new PlatformNotSupportedException();

        var arch = RuntimeInformation.OSArchitecture.ToString().ToLowerInvariant();
        var runtimePath = Path.Combine(AppContext.BaseDirectory, "runtimes", $"{os}-{arch}", "native");
        libraryPath = Path.Combine(runtimePath, AddLibraryExtension(libraryName));

        if (File.Exists(libraryPath))
        {
            AddToNativeSearchPath(runtimePath);
            PreloadOrtIfPresent(runtimePath);

            if (TryLoadNativeLibrary(libraryPath, out var handle))
            {
                LogResolution($"runtimes/{os}-{arch}/native", libraryPath);
                return handle;
            }
        }

        // 3. Fall back to standard OS search path
        LogResolution("OS search path (fallback)", AddLibraryExtension(libraryName));
        return IntPtr.Zero;
    }

    /// <summary>
    /// Emit a one-line marker to stderr (and the diagnostic trace) recording which strategy
    /// produced the loaded library and from where. Stderr is used so the line shows up in
    /// CI test-output capture even when no ILogger is wired in.
    /// </summary>
    private static void LogResolution(string strategy, string path)
    {
        var msg = $"[FoundryLocal.DllLoader] resolved foundry_local via {strategy}: {path}";
        Console.Error.WriteLine(msg);
        System.Diagnostics.Trace.WriteLine(msg);
    }

    /// <summary>
    /// Preload our onnxruntime then onnxruntime-genai by absolute path so foundry_local's
    /// imports bind to these copies. Best-effort: only takes effect if nothing else in the
    /// process has already loaded ORT — see class doc. Idempotent; missing files are skipped.
    /// </summary>
    private static void PreloadOrtIfPresent(string directory)
    {
        // ORT must be first as GenAI imports ORT.
        if (_ortHandle == IntPtr.Zero)
        {
            _ortHandle = TryPreload(directory, "onnxruntime");
        }

        if (_genAiHandle == IntPtr.Zero)
        {
            _genAiHandle = TryPreload(directory, "onnxruntime-genai");
        }
    }

    private static IntPtr TryPreload(string directory, string libraryName)
    {
        var path = Path.Combine(directory, AddLibraryExtension(libraryName));
        if (!File.Exists(path))
        {
            return IntPtr.Zero;
        }

        if (TryLoadNativeLibrary(path, out var handle))
        {
            var msg = $"[FoundryLocal.DllLoader] preloaded {libraryName}: {path}";
            Console.Error.WriteLine(msg);
            System.Diagnostics.Trace.WriteLine(msg);
            return handle;
        }

        var failMsg = $"[FoundryLocal.DllLoader] failed to preload {libraryName} from {path}";
        Console.Error.WriteLine(failMsg);
        System.Diagnostics.Trace.WriteLine(failMsg);
        return IntPtr.Zero;
    }
}