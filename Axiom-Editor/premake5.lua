group "Runtime"
project "Axiom-Editor"
    location "."
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    cdialect "C17"
    staticruntime "off"
    warnings "Extra"

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "src/**.cpp",
        "src/**.h",
        "src/**.hpp",
        "icon.rc"
    }

    -- Engine-level diagnostic overlays (StatsOverlay, LogOverlay, …).
    -- Same cross-binary share pattern as ProfilerPanel: the .cpp files use
    -- ImGui directly and are excluded from the engine DLL; we compile them
    -- into the editor binary so the Game View toolbar can drive them.
    files { path.join(ROOT_DIR, "Axiom-Engine/src/Diagnostics/**.cpp") }

    -- ImGuiImplBgfx now lives inside Axiom-Engine.dll. The editor used
    -- to compile its own copy from src/Gui/ImGuiImplBgfx.cpp here, but
    -- that gave the editor binary its own static-linked copy of bgfx
    -- — meaning the editor's `bgfx::getRendererType()` returned `Noop`
    -- even though engine.dll had brought up D3D11. Source moved into
    -- Axiom-Engine/src/Gui/ImGuiImplBgfx.cpp; the editor's
    -- ImGuiContextLayer.cpp now `#includes "Gui/ImGuiImplBgfx.hpp"`
    -- via the engine include path and links the AXIOM_API exports.

    UseDependencySet(Dependency.EditorRuntimeCommon)
    defines(GetAxiomModuleDefines())
    defines { "AIM_IMPORT_DLL" }
    includedirs { "src" }
    postbuildcommands { CopyAxiomAssets, CopyAxiomEngineDll, CopyGlfwDll, CopyGladDll }
    if AxiomProfiler.Enabled then postbuildcommands { CopyTracyDll } end

    filter "system:windows"
        buildoptions { "/utf-8", "/FS" }
        systemversion "latest"
        defines { "AIM_PLATFORM_WINDOWS" }
        postbuildcommands {
            '{COPYFILE} "' .. path.join(ROOT_DIR, "External/dotnet/lib/nethost.dll") .. '" "%{cfg.targetdir}/nethost.dll"'
        }

    filter "system:linux"
        defines { "AIM_PLATFORM_LINUX" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"
        defines { "AIM_DEBUG", "_DEBUG" }

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
        symbols "On"
        defines { "AIM_RELEASE", "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "Full"
        symbols "Off"
        defines { "AIM_DIST", "NDEBUG" }

    filter { "system:windows", "configurations:Dist" }
        kind "WindowedApp"

    filter {}

    -- Per-config libdirs for webgpu_dawn.lib (inherited via EngineCore-
    -- Render's Links). See ApplyDawnLibDirs in the root premake5.lua for
    -- the runtime-mismatch (LNK2038) rationale.
    ApplyDawnLibDirs("../")
