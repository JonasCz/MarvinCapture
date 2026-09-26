using System;
using System.IO;
using Microsoft.UI.Xaml;

namespace PinnacleCapture;

/// <summary>
/// Application entry point. Installs a global unhandled-exception handler that
/// logs to %LOCALAPPDATA%\PinnacleOSS\crash.log before anything else runs, so
/// even a P/Invoke DllNotFoundException from a missing pinnacle-oss-core.dll
/// gets recorded instead of producing a silent hard crash.
/// </summary>
public partial class App : Application
{
    public static string CrashLogPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "PinnacleOSS", "crash.log");

    private Window? _window;
    public static Window? MainAppWindow { get; private set; }

    public App()
    {
        InstallCrashLogging();
        InitializeComponent();
    }

    private void InstallCrashLogging()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(CrashLogPath)!);
        }
        catch
        {
            // best effort
        }

        AppDomain.CurrentDomain.UnhandledException += (s, e) =>
        {
            LogCrash("AppDomain.UnhandledException", e.ExceptionObject as Exception, e.IsTerminating);
        };

#if DEBUG
        if (Environment.GetEnvironmentVariable("PINNACLE_GUI_FIRSTCHANCE") == "1")
        {
            AppDomain.CurrentDomain.FirstChanceException += (s, e) =>
                LogCrash("FirstChance", e.Exception, false);
            DebugSettings.BindingFailed += (s, e) => LogCrash("BindingFailed: " + e.Message, null, false);
            DebugSettings.XamlResourceReferenceFailed += (s, e) => LogCrash("XamlResourceReferenceFailed: " + e.Message, null, false);
        }
#endif

        this.UnhandledException += (s, e) =>
        {
            LogCrash($"Application.UnhandledException: {e.Message}", e.Exception, false);
            // Leave e.Handled = false so default WinUI behavior (crash) still applies;
            // we only want to guarantee the log line exists first.
        };
    }

    /// <summary>Startup breadcrumbs, only with PINNACLE_GUI_FIRSTCHANCE=1 in Debug builds.</summary>
    [System.Diagnostics.Conditional("DEBUG")]
    public static void Trace(string what)
    {
        if (Environment.GetEnvironmentVariable("PINNACLE_GUI_FIRSTCHANCE") == "1")
        {
            LogCrash("trace: " + what, null, false);
        }
    }

    private static void LogCrash(string source, Exception? ex, bool terminating)
    {
        try
        {
            var line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] ({source}, terminating={terminating}) {ex}\n";
            File.AppendAllText(CrashLogPath, line);
        }
        catch
        {
            // If we can't log the crash, there's nothing else we can do here.
        }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // Probe the core before building any UI: a missing / mismatched
        // pinnacle-oss-core.dll becomes a logged, readable error window
        // rather than a hard crash from deep inside a view-model constructor.
        string? problem = null;
        try
        {
            uint v = Interop.Native.ApiVersion();
            if (v != 2)
            {
                problem = $"pinnacle-oss-core.dll implements API version {v}, but this app needs version 2.";
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            LogCrash("OnLaunched (core library probe)", ex, false);
            problem = "pinnacle-oss-core.dll could not be loaded. It must sit next to PinnacleCapture.exe " +
                      $"(with libwinpthread-1.dll for MinGW builds).\n\n{ex.Message}";
        }

        if (problem is not null)
        {
            _window = new Window { Title = "Pinnacle Capture" };
            _window.Content = new Microsoft.UI.Xaml.Controls.TextBlock
            {
                Text = problem,
                Margin = new Thickness(24),
                TextWrapping = TextWrapping.Wrap,
                IsTextSelectionEnabled = true,
            };
            MainAppWindow = _window;
            _window.Activate();
            return;
        }

        _window = new MainWindow();
        MainAppWindow = _window;
        _window.Activate();
    }
}
