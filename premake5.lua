workspace "Axiom"
    architecture "x64"
    startproject "Axiom-Launcher"

    configurations
    {
        "Debug",
        "Release",
        "Dist"
    }

    filter { "system:windows", "language:C++" }
        toolset "v143"

    filter {}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
ROOT_DIR = os.realpath(_MAIN_SCRIPT_DIR)

newoption
{
    trigger = "with-imgui-demo",
    description = "Include imgui_demo.cpp in the ImGui static library project."
}

newoption
{
    trigger = "module-profile",
    value = "PROFILE",
    description = "Axiom module profile: full (default compatibility), core, or custom. Supplying any --with-* option without this flag selects custom.",
    allowed =
    {
        { "full", "Compatibility profile: render, audio, physics, scripting, editor, and AXIOM_ALL_MODULES." },
        { "core", "Core-only profile: no optional module defines or third-party module dependencies." },
        { "custom", "Enable only the optional modules requested with --with-render/--with-audio/--with-physics/--with-scripting/--with-editor." }
    }
}

newoption { trigger = "with-render", description = "Enable the Axiom render module dependencies and AXIOM_WITH_RENDER." }
newoption { trigger = "with-audio", description = "Enable the Axiom audio module dependencies and AXIOM_WITH_AUDIO." }
newoption { trigger = "with-physics", description = "Enable the Axiom physics module dependencies and AXIOM_WITH_PHYSICS." }
newoption { trigger = "with-scripting", description = "Enable the Axiom scripting module dependencies and AXIOM_WITH_SCRIPTING." }
newoption { trigger = "with-editor", description = "Enable the Axiom editor module dependencies and AXIOM_WITH_EDITOR. This also enables render dependencies." }

newoption
{
    trigger = "axiom-project",
    value = "PATH",
    description = "Absolute path to an Axiom project. Adds <PATH>/Packages/<Name>/axiom-package.lua manifests to the package loader so project-local packages get built into the same solution."
}

newoption
{
    trigger = "no-profiler",
    description = "Strip the Axiom profiler entirely from the build. Without this flag, Tracy is compiled into the engine, AXIOM_PROFILER_ENABLED is defined for engine/editor/runtime, and the in-engine ImGui Profiler panel is available. Pass --no-profiler for shipped runtime builds with strict size budgets."
}

-- The engine has exactly one graphics backend (WebGPU via Dawn); the
-- runtime D3D12/Vulkan/Metal choice underneath Dawn is selected by Dawn
-- itself based on the adapter request in WebGPUApi.cpp::RequestAdapterSync.

-- Resolved early so subsequent dep-set wiring can branch on it. Default ON;
-- explicit --no-profiler turns it off. The single boolean drives:
--   - whether AXIOM_PROFILER_ENABLED is defined
--   - whether the Tracy project is included in the build
--   - whether the engine compiles src/Profiling/**
--   - whether the editor compiles src/Gui/ProfilerPanel.*
AxiomProfiler = {}
AxiomProfiler.Enabled = not _OPTIONS["no-profiler"]

require("premake/fix_csharp_platforms")

-- Surface DawnLayout (include / lib / define paths to the pre-built
-- webgpu_dawn.lib) BEFORE Dependencies.lua is loaded so EngineCoreRender
-- can splice the layout's lists into its own. dawn.lua is always loaded
-- — WebGPU is the only RHI, so a missing External/dawn/ checkout is a
-- real configuration error (scripts/SetupDawn.bat fixes it).
include "premake/dependencies/dawn.lua"

include "Dependencies.lua"

local function HasExplicitModuleOption()
    return _OPTIONS["with-render"]
        or _OPTIONS["with-audio"]
        or _OPTIONS["with-physics"]
        or _OPTIONS["with-scripting"]
        or _OPTIONS["with-editor"]
end

local function ResolveAxiomModules()
    local profile = _OPTIONS["module-profile"]
    if not profile then
        profile = HasExplicitModuleOption() and "custom" or "full"
    end

    local modules =
    {
        Profile = profile,
        FullCompatibility = profile == "full",
        Render = false,
        Audio = false,
        Physics = false,
        Scripting = false,
        Editor = false
    }

    if profile == "full" then
        modules.Render = true
        modules.Audio = true
        modules.Physics = true
        modules.Scripting = true
        modules.Editor = true
    elseif profile == "custom" then
        modules.Render = _OPTIONS["with-render"] or false
        modules.Audio = _OPTIONS["with-audio"] or false
        modules.Physics = _OPTIONS["with-physics"] or false
        modules.Scripting = _OPTIONS["with-scripting"] or false
        modules.Editor = _OPTIONS["with-editor"] or false
    end

    if modules.Editor then
        modules.Render = true
        modules.Audio = true
        modules.Physics = true
        modules.Scripting = true
    end

    return modules
end

AxiomModules = ResolveAxiomModules()

-- The engine itself no longer depends on ImGui (the editor application owns its own
-- ImGui context as a Layer). EngineCoreEditor is therefore consumed only by the
-- editor and launcher executables, not by the engine static library.
--
-- Exception: engine.dll hosts the WebGPU-side ImGui backend
-- (Gui/ImGuiImplWebGPU.cpp, a thin wrapper around the official
-- imgui_impl_wgpu). It touches the wgpu::Device that lives in
-- WebGPUApi.cpp's TU, so it has to compile in the same module. The
-- consumer's ImGuiContext is synced into engine.dll's ImGui copy through
-- PackageImGuiBridge at every backend entry point (same pattern packages
-- already use).
Dependency.EngineSelectedModules = MergeDependencySets(
    Dependency.EngineCore,
    AxiomModules.Render and Dependency.EngineCoreRender or nil,
    AxiomModules.Audio and Dependency.EngineCoreAudio or nil,
    AxiomModules.Physics and Dependency.EngineCorePhysics or nil,
    AxiomModules.Scripting and Dependency.EngineCoreScripting or nil,
    AxiomProfiler.Enabled and Dependency.Profiler or nil,
    Dependency.EngineCoreEditor  -- engine.dll always hosts the ImGui backend
)

Dependency.EditorRuntimeCommon = MergeDependencySets(
    {
        DependsOn = { "Axiom-Engine" },
        Links = { "Axiom-Engine" }
    },
    Dependency.EngineSelectedModules,
    AxiomModules.Editor and Dependency.EngineCoreEditor or nil
)

function GetAxiomModuleDefines()
    local hasApplication = AxiomModules.Render
        and AxiomModules.Audio
        and AxiomModules.Physics
        and AxiomModules.Scripting
        and AxiomModules.Editor

    local defines =
    {
        "AXIOM_WITH_RENDER=" .. (AxiomModules.Render and "1" or "0"),
        "AXIOM_WITH_AUDIO=" .. (AxiomModules.Audio and "1" or "0"),
        "AXIOM_WITH_PHYSICS=" .. (AxiomModules.Physics and "1" or "0"),
        "AXIOM_WITH_SCRIPTING=" .. (AxiomModules.Scripting and "1" or "0"),
        "AXIOM_WITH_EDITOR=" .. (AxiomModules.Editor and "1" or "0"),
        "AXIOM_WITH_APPLICATION=" .. (hasApplication and "1" or "0")
    }

    if AxiomModules.FullCompatibility then
        table.insert(defines, "AXIOM_ALL_MODULES=1")
    end

    return defines
end

function UseAxiomEngineModuleDependencies()
    UseDependencySet(Dependency.EngineSelectedModules)
end

-- Shared postbuild command: copy AxiomAssets into each target output
-- directory. The single source tree is `Axiom-Runtime/AxiomAssets/`,
-- staged via one {COPYDIR} next to the consumer exe.
CopyAxiomAssets = '{COPYDIR} "' .. path.join(ROOT_DIR, "Axiom-Runtime/AxiomAssets") .. '" "%{cfg.targetdir}/AxiomAssets"'

-- Shared postbuild command: copy the engine SharedLib next to each consumer executable
-- so it resolves at runtime without depending on PATH.
CopyAxiomEngineDll = '{COPYFILE} "' ..
    path.join(ROOT_DIR, "bin/" .. outputdir, "Axiom-Engine", "Axiom-Engine.dll") ..
    '" "%{cfg.targetdir}/Axiom-Engine.dll"'

-- GLFW and Glad are SharedLibs so all consumers (engine.dll + consumer .exes) share
-- one copy of their global state. Each consumer ships the DLLs alongside its binary.
CopyGlfwDll = '{COPYFILE} "' ..
    path.join(ROOT_DIR, "bin/" .. outputdir, "GLFW", "GLFW.dll") ..
    '" "%{cfg.targetdir}/GLFW.dll"'

-- Glad has been removed from the engine build (the render backend
-- handles its own GPU context). CopyGladDll stays defined as a no-op so
-- consumer postbuild lists referencing it don't have to be edited in
-- lockstep.
CopyGladDll = ""

-- Tracy is a SharedLib too (one client per process). Consumers ship the DLL.
-- When --no-profiler is set this expands to a no-op string the postbuild
-- list can still reference safely.
if AxiomProfiler and AxiomProfiler.Enabled then
    CopyTracyDll = '{COPYFILE} "' ..
        path.join(ROOT_DIR, "bin/" .. outputdir, "Tracy", "Tracy.dll") ..
        '" "%{cfg.targetdir}/Tracy.dll"'
else
    CopyTracyDll = "" -- no-op; consumer postbuild lists keep their structure
end

local function NormalizeRootPath(pathValue)
    if path.isabsolute(pathValue) then
        return pathValue
    end

    return path.join(ROOT_DIR, pathValue)
end

local function NormalizeRootPaths(paths)
    local normalized = {}

    for _, pathValue in ipairs(paths) do
        table.insert(normalized, NormalizeRootPath(pathValue))
    end

    return normalized
end

-- Apply per-config libdirs pointing at Dawn's Debug vs Release output
-- folders. Call once inside a project block; the filter context resets
-- afterwards so subsequent project config isn't accidentally scoped.
--
-- `rootRelPrefix` is the path prefix to prepend so the (repo-root-
-- relative) Dawn build directory resolves correctly from the consumer's
-- own premake working directory. Top-level project folders (Axiom-Engine/,
-- Axiom-Editor/, etc.) need "../"; deeper folders (Tests/Axiom-Engine-
-- Tests/) need "../../" — caller picks.
--
-- Why per-config: webgpu_dawn.lib lives in both `Debug/` and `Release/`
-- subdirectories with /MDd vs /MD runtime selection respectively. Linking
-- the wrong-config lib into an /MDd Debug engine produces LNK2038
-- "RuntimeLibrary" + "_ITERATOR_DEBUG_LEVEL" mismatch errors.
function ApplyDawnLibDirs(rootRelPrefix)
    local p = rootRelPrefix or "../"
    filter "configurations:Debug"
        libdirs { p .. "External/dawn/build/src/dawn/native/Debug" }
    filter "configurations:Release"
        libdirs { p .. "External/dawn/build/src/dawn/native/Release" }
    filter "configurations:Dist"
        libdirs { p .. "External/dawn/build/src/dawn/native/Release" }
    filter {}
end

function UseDependencySet(dep)
    if dep.IncludeDirs then
        includedirs(NormalizeRootPaths(dep.IncludeDirs))
    end

    if dep.LibDirs then
        libdirs(NormalizeRootPaths(dep.LibDirs))
    end

    if dep.DependsOn then
        dependson(dep.DependsOn)
    end

    if dep.Links then
        links(dep.Links)
    end

    if dep.Defines then
        defines(dep.Defines)
    end
end

group "Dependencies"
if AxiomModules.Editor then
    project "ImGui"
        location (path.join(ROOT_DIR, "premake/generated/ImGui"))
        kind "StaticLib"
        language "C++"
        cppdialect "C++20"
        staticruntime "off"

    targetdir (path.join(ROOT_DIR, "bin/" .. outputdir .. "/%{prj.name}"))
    objdir (path.join(ROOT_DIR, "bin-int/" .. outputdir .. "/%{prj.name}"))

    files
        {
            -- Core
            "External/imgui/imconfig.h",
            "External/imgui/imgui.h",
            "External/imgui/imgui_internal.h",
            "External/imgui/imstb_rectpack.h",
            "External/imgui/imstb_textedit.h",
            "External/imgui/imstb_truetype.h",
            "External/imgui/imgui.cpp",
            "External/imgui/imgui_draw.cpp",
            "External/imgui/imgui_tables.cpp",
            "External/imgui/imgui_widgets.cpp",

            -- Backends:
            --   * imgui_impl_glfw     — used by editor / launcher / runtime
            --     executables for input + window-event plumbing.
            --   * imgui_impl_opengl3  — used by those same executables for
            --     their own ImGui overlays (editor-managed panels, the
            --     launcher's project picker, runtime debug HUD). These run
            --     against a GLFW-owned GL context that's independent from
            --     the engine's WebGPU device.
            --   * imgui_impl_wgpu     — used by engine.dll via
            --     Gui/ImGuiImplWebGPU.cpp for in-engine ImGui draws on the
            --     shared wgpu::Device. Headers are reachable through
            --     Dependency.ImGui below (DawnLayout spliced in).
            "External/imgui/backends/imgui_impl_glfw.h",
            "External/imgui/backends/imgui_impl_glfw.cpp",
            "External/imgui/backends/imgui_impl_opengl3.h",
            "External/imgui/backends/imgui_impl_opengl3.cpp",
            "External/imgui/backends/imgui_impl_opengl3_loader.h",
            "External/imgui/backends/imgui_impl_wgpu.h",
            "External/imgui/backends/imgui_impl_wgpu.cpp"
        }

        if _OPTIONS["with-imgui-demo"] then
            files { "External/imgui/imgui_demo.cpp" }
        end

        vpaths
        {
            ["Core/*"] =
            {
                "External/imgui/imconfig.h",
                "External/imgui/imgui.h",
                "External/imgui/imgui_internal.h",
                "External/imgui/imstb_rectpack.h",
                "External/imgui/imstb_textedit.h",
                "External/imgui/imstb_truetype.h",
                "External/imgui/imgui.cpp",
                "External/imgui/imgui_draw.cpp",
                "External/imgui/imgui_tables.cpp",
                "External/imgui/imgui_widgets.cpp"
            },
            ["Backends/*"] =
            {
                "External/imgui/backends/imgui_impl_glfw.h",
                "External/imgui/backends/imgui_impl_glfw.cpp",
                "External/imgui/backends/imgui_impl_opengl3.h",
                "External/imgui/backends/imgui_impl_opengl3.cpp",
                "External/imgui/backends/imgui_impl_opengl3_loader.h",
                "External/imgui/backends/imgui_impl_wgpu.h",
                "External/imgui/backends/imgui_impl_wgpu.cpp"
            },
            ["Optional/*"] = { "External/imgui/imgui_demo.cpp" }
        }

        UseDependencySet(Dependency.ImGui)

        filter "system:windows"
            buildoptions { "/FS" }
            systemversion "latest"

        filter "configurations:Debug"
            runtime "Debug"
            symbols "On"
            defines { "_DEBUG" }

        filter "configurations:Release"
            runtime "Release"
            optimize "On"
            symbols "On"
            defines { "NDEBUG" }

        filter "configurations:Dist"
            runtime "Release"
            optimize "Full"
            symbols "Off"
            defines { "NDEBUG" }

        filter {}
end

if AxiomModules.Render then
    include "premake/dependencies/glfw.lua"
    -- Dawn (WebGPU) is the engine's sole GPU backend. The pre-built
    -- webgpu_dawn.lib comes from scripts/SetupDawn.bat — premake just
    -- links against it; see dawn.lua / DawnLayout for the paths.
    -- (No project here, no premake-side compilation — Dawn is too large
    -- to mirror into premake.)
end

if AxiomModules.Physics then
    include "premake/dependencies/box2d.lua"
    include "premake/dependencies/axiom_physics.lua"
end

if AxiomProfiler.Enabled then
    include "premake/dependencies/tracy.lua"
end

include "Axiom-Engine"

if AxiomModules.Scripting then
    include "Axiom-ScriptCore"
    include "Axiom-Sandbox"
end

if AxiomModules.Editor then
    include "Axiom-Editor"
end

if AxiomModules.FullCompatibility then
    include "Axiom-Launcher"
    include "Axiom-Runtime"
end

group ""

-- Tests live in their own group so they're easy to spot in the IDE solution
-- explorer and trivial to disable by commenting out this block.
group "Tests"
    include "Tests/Axiom-Engine-Tests"
group ""

-- Load any axiom-package.lua manifests under packages/ and register their projects.
local AxiomPackageLoader = dofile(path.join(ROOT_DIR, "premake/package-loader.lua"))
AxiomPackageLoader.LoadAll()
