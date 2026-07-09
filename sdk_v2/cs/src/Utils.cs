// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Threading.Tasks;

using Microsoft.Extensions.Logging;

internal class Utils
{
    // Helper to wrap function calls with consistent exception handling. Synchronous call to `func`.
    internal static T CallWithExceptionHandling<T>(Func<T> func, string errorMsg, ILogger logger)
    {
        try
        {
            return func();
        }
        // we ignore OperationCanceledException to allow proper cancellation propagation
        // this also covers TaskCanceledException since it derives from OperationCanceledException
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            if (ex is FoundryLocalException)
            {
                throw;
            }

            throw new FoundryLocalException(errorMsg, ex, logger);
        }
    }

    /// <summary>
    /// Wraps a synchronous func, runs it on a background thread observing the supplied
    /// cancellation token, and translates non-cancellation exceptions to
    /// <see cref="FoundryLocalException"/>. Use this for long-running native calls
    /// (catalog refresh, model download/load, web service start) so they don't block
    /// the caller's thread and respect cancellation.
    /// </summary>
    internal static async Task<T> CallWithExceptionHandlingAsync<T>(
        Func<T> func, string errorMsg, ILogger logger, CancellationToken? ct = null)
    {
        try
        {
            return await Task.Run(func, ct ?? CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            if (ex is FoundryLocalException)
            {
                throw;
            }

            throw new FoundryLocalException(errorMsg, ex, logger);
        }
    }

    /// <summary>
    /// Background-thread overload returning Task (no result).
    /// </summary>
    internal static async Task CallWithExceptionHandlingAsync(
        Action action, string errorMsg, ILogger logger, CancellationToken? ct = null)
    {
        try
        {
            await Task.Run(action, ct ?? CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            if (ex is FoundryLocalException)
            {
                throw;
            }

            throw new FoundryLocalException(errorMsg, ex, logger);
        }
    }

    /// <summary>
    /// Awaitable variant for callers whose work is already async.
    /// </summary>
    internal static async Task<T> CallWithExceptionHandlingAsync<T>(
        Func<Task<T>> func, string errorMsg, ILogger logger)
    {
        try
        {
            return await func().ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is not OperationCanceledException && ex is not ArgumentException)
        {
            if (ex is FoundryLocalException)
            {
                throw;
            }

            throw new FoundryLocalException(errorMsg, ex, logger);
        }
    }

    /// <summary>
    /// Awaitable variant returning Task (no result).
    /// </summary>
    internal static async Task CallWithExceptionHandlingAsync(
        Func<Task> func, string errorMsg, ILogger logger)
    {
        try
        {
            await func().ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is not OperationCanceledException && ex is not ArgumentException)
        {
            if (ex is FoundryLocalException)
            {
                throw;
            }

            throw new FoundryLocalException(errorMsg, ex, logger);
        }
    }

    /// <summary>
    /// Wraps an <see cref="IAsyncEnumerable{T}"/> producer so that exceptions thrown during
    /// enumeration are translated to <see cref="FoundryLocalException"/> consistent with
    /// <see cref="CallWithExceptionHandling{T}"/>. Cancellation propagates unchanged.
    /// </summary>
    internal static async IAsyncEnumerable<T> WrapStreamingExceptions<T>(
        IAsyncEnumerable<T> source, string errorMsg, ILogger logger,
        [System.Runtime.CompilerServices.EnumeratorCancellation] CancellationToken ct = default)
    {
        await using var enumerator = source.GetAsyncEnumerator(ct);

        while (true)
        {
            T item;

            try
            {
                if (!await enumerator.MoveNextAsync().ConfigureAwait(false))
                {
                    yield break;
                }

                item = enumerator.Current;
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (FoundryLocalException)
            {
                throw;
            }
            catch (Exception ex)
            {
                throw new FoundryLocalException(errorMsg, ex, logger);
            }

            yield return item;
        }
    }
}
