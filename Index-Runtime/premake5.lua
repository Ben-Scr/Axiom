project "Index-Runtime"
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
    if IndexProfiler.Enabled then
        files { path.join(ROOT_DIR, "Index-Editor/src/Gui/ProfilerPanel.cpp") }
        includedirs { path.join(ROOT_DIR, "Index-Editor/src") }
    end

    -- Engine-level diagnostic overlays (StatsOverlay = F6, LogOverlay = F7,
    -- and any future additions). Glob picks up new files automatically.
    -- Same cross-binary share as ProfilerPanel: the .cpp files use ImGui
    -- and are excluded from the engine DLL; we compile them into the
    -- runtime exe so the F6/F7 toggle layers can drive them.
    files { path.join(ROOT_DIR, "Index-Engine/src/Diagnostics/**.cpp") }

    -- The ImGui WebGPU backend lives inside Index-Engine.dll so the
    -- runtime exe and engine share one wgpu::Device. RuntimeImGuiHost.cpp
    -- `#includes "Gui/ImGuiImplWebGPU.hpp"` from the engine include path
    -- and links the INDEX_API exports.

    UseDependencySet(Dependency.EditorRuntimeCommon)
    defines(GetIndexModuleDefines())
    defines { "IDX_IMPORT_DLL" }
    -- See WriteIndexEntityBitsConfigHeader() in the root premake5.lua.
    includedirs { IndexEntityBitsConfigIncludeDir }

    -- ScriptSystem loads Index-ScriptCore.dll at runtime. The packaged-layout
    -- branch (ScriptSystem.cpp:194) finds it next to the runtime exe; the
    -- copy lets a zipped Runtime folder work standalone.
    dependson { "Index-ScriptCore" }
    postbuildcommands
    {
        CopyIndexAssets,
        CopyIndexEngineDll,
        CopyScriptCoreDll,
        CopyGlfwDll,
        CopyGladDll
    }
    if IndexProfiler.Enabled then postbuildcommands { CopyTracyDll } end

    filter "system:windows"
        -- See Index-Engine/premake5.lua for the rationale on
        -- MultiProcessorCompile + /Zc:preprocessor.
        flags { "MultiProcessorCompile" }
        buildoptions { "/utf-8", "/FS", "/Zc:preprocessor" }
        systemversion "latest"
        links { "%{Library.GDI32}" }
        defines { "IDX_PLATFORM_WINDOWS" }
        postbuildcommands {
            '{COPYFILE} "' .. path.join(ROOT_DIR, "External/dotnet/lib/nethost.dll") .. '" "%{cfg.targetdir}/nethost.dll"'
        }

    filter "system:linux"
        defines { "IDX_PLATFORM_LINUX" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"
        defines { "IDX_DEBUG", "_DEBUG" }

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
        symbols "On"
        defines { "IDX_RELEASE", "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "Full"
        symbols "Off"
        defines { "IDX_DIST", "NDEBUG" }

    filter { "system:windows", "configurations:Dist" }
        kind "WindowedApp"

    filter {}

    -- Per-config libdirs for webgpu_dawn.lib (inherited via EngineCore-
    -- Render's Links). See ApplyDawnLibDirs in the root premake5.lua for
    -- the runtime-mismatch (LNK2038) rationale.
    ApplyDawnLibDirs("../")
