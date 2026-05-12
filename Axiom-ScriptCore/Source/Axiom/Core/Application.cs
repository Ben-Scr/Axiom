using System;
using System.IO;

namespace Axiom;

/// <summary>
/// Provides static access to application-level runtime information
/// such as timing, screen dimensions, and lifecycle control.
/// </summary>
public static class Application
{
    public static event Action? OnPaused;
    public static event Action? OnStart;
    public static event Action? OnQuit;

    internal static void RaiseApplicationPaused() => OnPaused?.Invoke();
    internal static void RaiseApplicationStart() => OnStart?.Invoke();
    internal static void RaiseApplicationQuit() => OnQuit?.Invoke();

    public static float TargetFrameRate
    {
        get => InternalCalls.Application_GetTargetFrameRate();
        set => InternalCalls.Application_SetTargetFrameRate(value);
    }

    public static bool VsyncEnabled
    {
        get => InternalCalls.Application_GetVsyncEnabled();
        set => InternalCalls.Application_SetVsyncEnabled(value);
    }

    /// <summary>
    /// The path to the persistent data directory of the application.
    /// Use this to store save files, settings, or any data that should
    /// persist across sessions. The directory is guaranteed to be writable.
    /// </summary>
    public static string PersistentDataPath => EnsureDirectory(
        Path.Combine(GetLocalApplicationDataPath(), "Axiom"));

    /// <summary>
    /// The path to the directory where the application is installed.
    /// </summary>
    public static string DataPath => AppContext.BaseDirectory;

    /// <summary>
    /// The temporary cache directory. Data here may be cleared by the OS.
    /// </summary>
    public static string TemporaryCachePath => EnsureDirectory(
        Path.Combine(Path.GetTempPath(), "Axiom"));

    public static int ScreenWidth => InternalCalls.Application_GetScreenWidth();
    public static int ScreenHeight => InternalCalls.Application_GetScreenHeight();
    public static float AspectRatio => ScreenHeight > 0 ? (float)ScreenWidth / ScreenHeight : 1.0f;

    public static bool RunningInBackground
    {
        get => throw new System.NotImplementedException();
        set => throw new System.NotImplementedException();
    }

    /// <summary>
    /// True when the script is running inside the editor process (editor
    /// preview / play-mode), false when running in a built game.
    ///
    /// Runtime sibling of the compile-time AXIOM_EDITOR define — both are
    /// available so callers can pick whichever fits the use:
    /// <code>
    /// #if AXIOM_EDITOR                    // strips at compile time
    ///     EditorOnlyDebugDraw();
    /// #endif
    ///
    /// if (Application.IsEditor) {         // runtime branch (no #if)
    ///     SkipFatalErrorsThatWouldKillTheEditorProcess();
    /// }
    /// </code>
    /// </summary>
    public static bool IsEditor => InternalCalls.Application_IsEditor();

    /// <summary>
    /// The current OS clipboard contents as a UTF-8 string. Reads/writes the
    /// system clipboard via the engine's GLFW window. Returns an empty string
    /// in headless contexts (no window); the setter is a no-op there.
    /// </summary>
    public static string ClipboardString
    {
        get => InternalCalls.Application_GetClipboardString();
        set => InternalCalls.Application_SetClipboardString(value);
    }

    /// <summary>
    /// Quits the application. Only works in build mode, ignored in the editor.
    /// </summary>
    public static void Quit() => InternalCalls.Application_Quit();

    private static string GetLocalApplicationDataPath()
    {
        string path = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return string.IsNullOrEmpty(path) ? AppContext.BaseDirectory : path;
    }

    private static string EnsureDirectory(string path)
    {
        Directory.CreateDirectory(path);
        return path;
    }
}
