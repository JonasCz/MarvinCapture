using System;
using System.IO;
using Microsoft.UI.Xaml;

namespace PinnacleCapture;

/// <summary>
/// Application entry point. Installs a global unhandled-exception handler that
/// logs to %LOCALAPPDATA%\PinnacleOSS\crash.log before anything else runs, so
/// even a P/Invoke DllNotFoundException from a missing marvin-core.dll
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
        ConsoleOutput.Init();
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

#if false // first-chance exception / binding-failure tracing (was PINNACLE_GUI_FIRSTCHANCE=1 in Debug builds): change to #if DEBUG to bring it back
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

    /// <summary>Startup breadcrumbs: disabled (was PINNACLE_GUI_FIRSTCHANCE=1 in Debug builds); remove the #if false to bring it back.</summary>
    [System.Diagnostics.Conditional("DEBUG")]
    public static void Trace(string what)
    {
#if false
        LogCrash("trace: " + what, null, false);
#endif
    }

    private static void LogCrash(string source, Exception? ex, bool terminating)
    {
        try
        {
            var line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] ({source}, terminating={terminating}) {ex}\n";
            ConsoleOutput.Write($"{source}: {ex}");
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
        // marvin-core.dll becomes a logged, readable error window
        // rather than a hard crash from deep inside a view-model constructor.
        string? problem = null;
        try
        {
            uint v = Interop.Native.ApiVersion();
            if (v != 3)
            {
                problem = $"marvin-core.dll implements API version {v}, but this app needs version 3.";
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            LogCrash("OnLaunched (core library probe)", ex, false);
            problem = "marvin-core.dll could not be loaded. It must sit next to MarvinCaptureGUI.exe " +
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
