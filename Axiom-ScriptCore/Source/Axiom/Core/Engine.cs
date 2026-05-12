using Axiom.Interop;

namespace Axiom;

/// <summary>
/// Build configuration the script is running under.
/// Mirrors the AXIOM_BUILD_* preprocessor symbols emitted at script-compile time.
/// </summary>
public enum BuildConfiguration
{
    /// <summary>Editor preview / iteration mode.</summary>
    Debug = 0,
    /// <summary>Built game with diagnostics enabled (AXIOM_BUILD_DEVELOPMENT).</summary>
    Development = 1,
    /// <summary>Shipped game build (AXIOM_BUILD_RELEASE).</summary>
    Release = 2,
}

public enum Platform
{
    Windows,
    Linux,
    MacOS,
    Android,
    iOS,
    WebGL,
    Unknown
}

public enum GraphicsApi
{
    Direct3D11,
    Direct3D12,
    Vulkan,
    Metal,
    OpenGL,
    Unknown
}

/// <summary>
/// Engine-level identity and graphics-capability info.
/// Read-only; values are determined at process start.
/// </summary>
public static class Engine
{
    /// <summary>Engine semver string, e.g. "2026.1.0".</summary>
    public static string Version => InternalCalls.Engine_GetVersion();

    /// <summary>Full engine version string, e.g. "Axiom 2026.1.0 (Windows x64 Release)".
    /// Mirrors the AIM_VERSION_LONG macro on the C++ side.</summary>
    public static string VersionLong => InternalCalls.Engine_GetVersionLong();
    public static string ConfigurationName => throw new NotImplementedException("Engine.ConfigurationName is not implemented yet.");

    /// <summary>
    /// Active build configuration (Debug / Development / Release). Mirrors
    /// the AXIOM_BUILD_* defines but resolved at runtime so callers don't
    /// need #if directives.
    /// </summary>
    public static BuildConfiguration BuildConfiguration
        => (BuildConfiguration)InternalCalls.Engine_GetBuildConfiguration();

    /// <summary>OS name, e.g. "Windows" or "Linux".</summary>
    public static string Platform => InternalCalls.Engine_GetPlatform();

    /// <summary>
    /// Graphics backend label, e.g. "WebGPU Vulkan" or "WebGPU Direct3D12".
    /// Empty string before the renderer is initialized.
    /// </summary>
    public static string GraphicsApi => InternalCalls.Engine_GetGraphicsApi();

    /// <summary>GPU vendor name, e.g. "NVIDIA", "AMD", "Intel".</summary>
    public static string GpuVendor => InternalCalls.Engine_GetGpuVendor();
    public static string CpuVendor => throw new NotImplementedException();

    /// <summary>Active renderer backend name, e.g. "Vulkan", "Direct3D12", "Metal".</summary>
    public static string GpuRenderer => InternalCalls.Engine_GetGpuRenderer();

}
