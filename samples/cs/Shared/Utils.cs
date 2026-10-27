using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging;
using System.Text;

internal static class Utils
{
    private static readonly ILoggerFactory _loggerFactory;

    static Utils()
    {
        _loggerFactory = Microsoft.Extensions.Logging.LoggerFactory.Create(builder =>
        {
            builder.SetMinimumLevel(Microsoft.Extensions.Logging.LogLevel.Information);
        });
    }

    /// <summary>
    /// Get a dummy application logger.
    /// </summary>
    /// <returns>ILogger</returns>
    internal static ILogger GetAppLogger()
    {
        return _loggerFactory.CreateLogger("FoundryLocalSamples");
    }

    internal static async Task RunWithSpinner<T>(string msg, T workTask, bool warnOnException = true) where T : Task
    {
        // Start the spinner
        using var cts = new CancellationTokenSource();
        var spinnerTask = ShowSpinner(msg, cts.Token);

        try
        {
            await workTask;     // wait for the real work to finish
        }
        catch (Exception fex)
        {
            // we're only using this for EP registration currently an exception here is non-fatal as we have built-in
            // execution providers that can be used. in a production app you may want to handle this differently.
            if (warnOnException)
            {
                cts.Cancel();
                Console.WriteLine($"\nWarning: {fex.Message}");
                return;
            }
            
            throw;  // rethrow otherwise
        }
        
        cts.Cancel();       // stop the spinner
        await spinnerTask;  // wait for spinner to exit
    }

    private static async Task ShowSpinner(string msg, CancellationToken token)
    {
        if (Console.IsOutputRedirected)
        {
            Console.WriteLine($"{msg}...");
            return;
        }

        Console.OutputEncoding = Encoding.UTF8;

        var sequence = new[] { '◴','◷','◶','◵' };

        int counter = 0;

        while (!token.IsCancellationRequested)
        {
            Console.SetCursorPosition(0, Console.CursorTop);
            Console.Write($"{msg}... {sequence[counter % sequence.Length]}\t");
            counter++;
            await Task.Delay(200, token).ContinueWith(_ => { });
        }

        Console.WriteLine($"Done.\n");
    }

    // Example tool method
    internal static int MultiplyNumbers(int first, int second)
    {
        return first * second;
    }
}