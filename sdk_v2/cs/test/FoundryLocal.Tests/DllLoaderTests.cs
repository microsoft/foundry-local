// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

using LoggingLevel = Microsoft.Extensions.Logging.LogLevel;

[NotInParallel]
internal sealed class DllLoaderTests
{
    [Test]
    [Arguments(LoggingLevel.None)]
    [Arguments(LoggingLevel.Warning)]
    [Arguments(LoggingLevel.Debug)]
    public async Task Diagnostics_RespectLoggerWithoutWritingToConsoleOrTrace(LoggingLevel minimumLevel)
    {
        var previousLogger = GetLogger();
        var previousOut = Console.Out;
        var previousError = Console.Error;
        using var stdout = new StringWriter();
        using var stderr = new StringWriter();
        using var trace = new StringWriter();
        using var listener = new TextWriterTraceListener(trace);
        var logger = new RecordingLogger(minimumLevel);
        var directory = Path.Combine(AppContext.BaseDirectory, "TestResults", "dll-loader-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var invalidLibrary = Path.Combine(directory, LibraryFileName("invalid"));
        File.WriteAllText(invalidLibrary, "Not a native library.");

        try
        {
#pragma warning disable TUnit0055 // Serialized console-output regression test; writers are restored in finally.
            Console.SetOut(stdout);
            Console.SetError(stderr);
#pragma warning restore TUnit0055
            Trace.Listeners.Add(listener);
            DllLoader.Initialize(NullLogger.Instance);
            DllLoader.Initialize(minimumLevel == LoggingLevel.None ? NullLogger.Instance : logger);

            InvokeLoader<object?>("LogResolution", "BaseDirectory", "test-path");
            InvokeLoader<object?>("LogResolution", "OS search path (fallback)", LibraryFileName("foundry_local"));

            // Exercise the successful preload path with the runtime already loaded in this process.
            var runtimeLibrary = RuntimeInformation.FrameworkDescription.StartsWith(".NET Framework", StringComparison.Ordinal)
                ? "clr" : "coreclr";
            var handle = InvokeLoader<IntPtr>("TryPreload", RuntimeEnvironment.GetRuntimeDirectory(), runtimeLibrary);
            await Assert.That(handle != IntPtr.Zero).IsTrue();
            FreeNativeLibrary(handle);

            await Assert.That(InvokeLoader<IntPtr>("TryPreload", directory, "invalid")).IsEqualTo(IntPtr.Zero);
            await Assert.That(InvokeLoader<IntPtr>("TryPreload", directory, "missing")).IsEqualTo(IntPtr.Zero);
        }
        finally
        {
            Trace.Listeners.Remove(listener);
#pragma warning disable TUnit0055 // Restore the test runner's writers after the serialized output capture.
            Console.SetOut(previousOut);
            Console.SetError(previousError);
#pragma warning restore TUnit0055
            DllLoader.Initialize(previousLogger);
            File.Delete(invalidLibrary);
            Directory.Delete(directory);
        }

        await Assert.That(stdout.ToString()).IsEqualTo("");
        await Assert.That(stderr.ToString()).IsEqualTo("");
        await Assert.That(trace.ToString()).IsEqualTo("");

        var expectedCount = minimumLevel == LoggingLevel.None ? 0 : minimumLevel == LoggingLevel.Warning ? 1 : 4;
        await Assert.That(logger.Entries.Count).IsEqualTo(expectedCount);
        if (minimumLevel != LoggingLevel.None)
        {
            var warning = logger.Entries[logger.Entries.Count - 1];
            await Assert.That(warning.Level).IsEqualTo(LoggingLevel.Warning);
            await Assert.That(warning.Message).Contains("failed to preload invalid");
            await Assert.That(warning.Properties["LibraryName"]).IsEqualTo("invalid");
            await Assert.That(warning.Properties["Path"]).IsEqualTo(invalidLibrary);
        }

        if (minimumLevel == LoggingLevel.Debug)
        {
            await Assert.That(logger.Entries[0].Level).IsEqualTo(LoggingLevel.Debug);
            await Assert.That(logger.Entries[0].Properties["Strategy"]).IsEqualTo("BaseDirectory");
            await Assert.That(logger.Entries[1].Message).Contains("OS search path (fallback)");
            await Assert.That(logger.Entries[2].Level).IsEqualTo(LoggingLevel.Debug);
            await Assert.That(logger.Entries[2].Message).Contains("preloaded");
        }
    }

    [Test]
    public async Task Initialize_Again_UsesCurrentLogger()
    {
        var previousLogger = GetLogger();
        var first = new RecordingLogger(LoggingLevel.Debug);
        var second = new RecordingLogger(LoggingLevel.Debug);

        try
        {
            DllLoader.Initialize(first);
            InvokeLoader<object?>("LogResolution", "first", "first-path");
            var firstCount = first.Entries.Count;
            DllLoader.Initialize(second);
            DllLoader.Initialize();
            InvokeLoader<object?>("LogResolution", "second", "second-path");
            DllLoader.Initialize(NullLogger.Instance);
            InvokeLoader<object?>("LogResolution", "discarded", "discarded-path");

            await Assert.That(first.Entries.Count).IsEqualTo(firstCount);
            await Assert.That(second.Entries.Count).IsEqualTo(1);
            await Assert.That(second.Entries[0].Properties["Strategy"]).IsEqualTo("second");
        }
        finally
        {
            DllLoader.Initialize(previousLogger);
        }
    }

    private static ILogger GetLogger() =>
        (ILogger)typeof(DllLoader).GetField("_logger", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;

    private static T InvokeLoader<T>(string method, params object[] arguments) =>
        (T)typeof(DllLoader).GetMethod(method, BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, arguments)!;

    private static string LibraryFileName(string name) =>
        RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? name + ".dll" :
        RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? "lib" + name + ".dylib" : "lib" + name + ".so";

    private static void FreeNativeLibrary(IntPtr handle)
    {
#if NET8_0_OR_GREATER
        NativeLibrary.Free(handle);
#else
        if (!FreeLibrary(handle))
        {
            throw new InvalidOperationException("Failed to release the test library handle.");
        }
#endif
    }

#if !NET8_0_OR_GREATER
    [DllImport("kernel32", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool FreeLibrary(IntPtr handle);
#endif

    private sealed class RecordingLogger(LoggingLevel minimumLevel) : ILogger
    {
        internal List<(LoggingLevel Level, string Message, Dictionary<string, object?> Properties)> Entries { get; } = [];

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LoggingLevel logLevel) => logLevel >= minimumLevel && minimumLevel != LoggingLevel.None;

        public void Log<TState>(LoggingLevel logLevel, EventId eventId, TState state, Exception? exception,
                                Func<TState, Exception?, string> formatter)
        {
            if (IsEnabled(logLevel))
            {
                var properties = ((IEnumerable<KeyValuePair<string, object?>>)state!).ToDictionary(p => p.Key, p => p.Value);
                Entries.Add((logLevel, formatter(state, exception), properties));
            }
        }
    }
}
