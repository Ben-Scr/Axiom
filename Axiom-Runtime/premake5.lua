project "Axiom-Runtime"
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

    -- When the profiler is enabled the runtime hosts the same ProfilerPanel
    -- the editor uses. We compile its .cpp into the runtime binary directly
    -- (rather than splitting it into a third project) and add the editor's
    -- src as an include dir so the panel header resolves. Stays out of the
    -- runtime entirely when --no-profiler strips the macro.
    if AxiomProfiler.Enabled then
        files { path.join(ROOT_DIR, "Axiom-Editor/src/Gui/ProfilerPanel.cpp") }
        includedirs { path.join(ROOT_DIR, "Axiom-Editor/src") }
    end

    -- Engine-level diagnostic overlays (StatsOverlay = F6, LogOverlay = F7,
    -- and any future additions). Glob picks up new files automatically.
    -- Same cross-binary share as ProfilerPanel: the .cpp files use ImGui
    -- and are excluded from the engine DLL; we compile them into the
    -- runtime exe so the F6/F7 toggle layers can drive them.
    files { path.join(ROOT_DIR, "Axiom-Engine/src/Diagnostics/**.cpp") }

    UseDependencySet(Dependency.EditorRuntimeCommon)
    defines(GetAxiomModuleDefines())
    defines { "AIM_IMPORT_DLL" }

    postbuildcommands
    {
        CopyAxiomAssets,
        CopyAxiomEngineDll,
        CopyGlfwDll,
        CopyGladDll
    }
    if AxiomProfiler.Enabled then postbuildcommands { CopyTracyDll } end

    filter "system:windows"
        buildoptions { "/utf-8", "/FS" }
        systemversion "latest"
        links { "%{Library.GDI32}" }
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
