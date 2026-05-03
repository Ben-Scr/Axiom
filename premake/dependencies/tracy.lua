-- Tracy profiler client library.
--
-- Built only when the profiler is enabled (root premake5.lua sets
-- AxiomProfiler.Enabled = true unless --no-profiler was passed). Compiled
-- as a SharedLib so the engine.dll, editor.exe, and runtime.exe all share
-- one Tracy client (Tracy is meant to have exactly one client per process,
-- and our three-binary split would otherwise risk duplicate registration).
--
-- Tracy v0.11.1's "fast integration" path is to compile a single source
-- file: External/tracy/public/TracyClient.cpp. Everything else is pulled
-- in by that TU.

project "Tracy"
    location (path.join(ROOT_DIR, "premake/generated/Tracy"))
    kind "SharedLib"
    language "C++"
    cppdialect "C++17"
    staticruntime "off"

    targetdir (path.join(ROOT_DIR, "bin/" .. outputdir .. "/%{prj.name}"))
    objdir (path.join(ROOT_DIR, "bin-int/" .. outputdir .. "/%{prj.name}"))

    files
    {
        path.join(ROOT_DIR, "External/tracy/public/TracyClient.cpp")
    }

    includedirs
    {
        path.join(ROOT_DIR, "External/tracy/public")
    }

    -- TRACY_ENABLE turns the macros into real instrumentation.
    -- TRACY_EXPORTS marks symbols dllexport on the build side; consumers
    -- get TRACY_IMPORTS to flip them to dllimport.
    -- TRACY_ON_DEMAND defers Tracy's internal sampling until a viewer
    -- actually connects over TCP. Without this, Tracy starts buffering
    -- zone events from process startup — even when no viewer is running —
    -- and the buffer grows indefinitely (we observed multi-GB RAM growth
    -- in long editor sessions). On-demand mode means: zero memory cost
    -- when no viewer is attached; full fidelity once one connects.
    -- IMPORTANT: this define must also be set on consumers (engine,
    -- editor, runtime, tests) so the Tracy headers compile the same
    -- code-paths on both sides — see Dependency.Profiler in
    -- Dependencies.lua for the consumer side.
    defines
    {
        "TRACY_ENABLE",
        "TRACY_EXPORTS",
        "TRACY_ON_DEMAND"
    }

    filter "system:windows"
        -- /Zc:preprocessor: enables MSVC's standards-conformant preprocessor.
        -- Required by Tracy v0.11+ on MSVC because TracyETW.cpp uses
        -- __LINE__ inside a `static constexpr SourceLocationData{...}` aggregate
        -- initialiser. MSVC's traditional preprocessor expands __LINE__ to
        -- __LINE__Var (a non-constant intermediate) which fails constexpr
        -- evaluation (C2131). The conformant preprocessor expands it to a
        -- proper integer literal.
        buildoptions { "/FS", "/Zc:preprocessor" }
        systemversion "latest"
        defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:linux"
        pic "On"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
        symbols "On"

    filter "configurations:Dist"
        runtime "Release"
        optimize "Full"
        symbols "Off"
