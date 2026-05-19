workspace "Index"
    architecture "x64"
    startproject "Index-Launcher"

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
    description = "Index module profile: full (default compatibility), core, or custom. Supplying any --with-* option without this flag selects custom.",
    allowed =
    {
        { "full", "Compatibility profile: render, audio, physics, scripting, editor, and INDEX_ALL_MODULES." },
        { "core", "Core-only profile: no optional module defines or third-party module dependencies." },
        { "custom", "Enable only the optional modules requested with --with-render/--with-audio/--with-physics/--with-scripting/--with-editor." }
    }
}

newoption { trigger = "with-render", description = "Enable the Index render module dependencies and INDEX_WITH_RENDER." }
newoption { trigger = "with-audio", description = "Enable the Index audio module dependencies and INDEX_WITH_AUDIO." }
newoption { trigger = "with-physics", description = "Enable the Index physics module dependencies and INDEX_WITH_PHYSICS." }
newoption { trigger = "with-scripting", description = "Enable the Index scripting module dependencies and INDEX_WITH_SCRIPTING." }
newoption { trigger = "with-editor", description = "Enable the Index editor module dependencies and INDEX_WITH_EDITOR. This also enables render dependencies." }

newoption
{
    trigger = "index-project",
    value = "PATH",
    description = "Absolute path to an Index project. Adds <PATH>/Packages/<Name>/index-package.lua manifests to the package loader so project-local packages get built into the same solution."
}

newoption
{
    trigger = "no-profiler",
    description = "Strip the Index profiler entirely from the build. Without this flag, Tracy is compiled into the engine, INDEX_PROFILER_ENABLED is defined for engine/editor/runtime, and the in-engine ImGui Profiler panel is available. Pass --no-profiler for shipped runtime builds with strict size budgets."
}

-- The engine has exactly one graphics backend (WebGPU via Dawn); the
-- runtime D3D12/Vulkan/Metal choice underneath Dawn is selected by Dawn
-- itself based on the adapter request in WebGPUApi.cpp::RequestAdapterSync.

-- Resolved early so subsequent dep-set wiring can branch on it. Default ON;
-- explicit --no-profiler turns it off. The single boolean drives:
--   - whether INDEX_PROFILER_ENABLED is defined
--   - whether the Tracy project is included in the build
--   - whether the engine compiles src/Profiling/**
--   - whether the editor compiles src/Gui/ProfilerPanel.*
IndexProfiler = {}
IndexProfiler.Enabled = not _OPTIONS["no-profiler"]

require("premake/fix_csharp_platforms")

-- Surface DawnLayout (include / lib / define paths to the pre-built
-- webgpu_dawn.lib) BEFORE Dependencies.lua is loaded so EngineCoreRender
-- can splice the layout's lists into its own. dawn.lua is always loaded
-- — WebGPU is the only RHI, so a missing External/dawn/ checkout is a
-- real configuration error (scripts/dawn/SetupDawn.bat fixes it).
include "premake/dependencies/dawn.lua"

include "Dependencies.lua"

local function HasExplicitModuleOption()
    return _OPTIONS["with-render"]
        or _OPTIONS["with-audio"]
        or _OPTIONS["with-physics"]
        or _OPTIONS["with-scripting"]
        or _OPTIONS["with-editor"]
end

local function ResolveIndexModules()
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

IndexModules = ResolveIndexModules()

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
    IndexModules.Render and Dependency.EngineCoreRender or nil,
    IndexModules.Audio and Dependency.EngineCoreAudio or nil,
    IndexModules.Physics and Dependency.EngineCorePhysics or nil,
    IndexModules.Scripting and Dependency.EngineCoreScripting or nil,
    IndexProfiler.Enabled and Dependency.Profiler or nil,
    Dependency.EngineCoreEditor  -- engine.dll always hosts the ImGui backend
)

Dependency.EditorRuntimeCommon = MergeDependencySets(
    {
        DependsOn = { "Index-Engine" },
        Links = { "Index-Engine" }
    },
    Dependency.EngineSelectedModules,
    IndexModules.Editor and Dependency.EngineCoreEditor or nil
)

-- Picks the EnTT entity/version bit-split from the active project's
-- index-project.json. Default is 20 — matches EnTT's stock split, so
-- existing projects pay no extra memory and behave identically to the
-- unpatched engine. Cached so repeated calls from each consumer's
-- premake5.lua don't re-parse the file. The vendored EnTT header at
-- External/entt/src/entt/entity/entity.hpp branches on the resulting
-- -DINDEX_ENTITY_BITS=N to adjust entt_traits<uint32_t>::entity_mask /
-- version_mask.
--
-- Mirrors the IndexProject::Load validator in
-- Index-Engine/src/Project/IndexProject.cpp so the premake-time and
-- runtime views of the setting stay in sync.
IndexEntityBits = nil
function GetIndexEntityBits()
    if IndexEntityBits ~= nil then
        return IndexEntityBits
    end

    local default = 20
    local projectRootDir = _OPTIONS["index-project"]
    if not projectRootDir or projectRootDir == "" then
        IndexEntityBits = default
        return IndexEntityBits
    end

    local manifestPath = path.join(projectRootDir, "index-project.json")
    if not os.isfile(manifestPath) then
        IndexEntityBits = default
        return IndexEntityBits
    end

    local file = io.open(manifestPath, "rb")
    if not file then
        IndexEntityBits = default
        return IndexEntityBits
    end
    local jsonText = file:read("*all")
    file:close()
    if not jsonText then
        IndexEntityBits = default
        return IndexEntityBits
    end

    jsonText = jsonText:gsub("//[^\n]*", ""):gsub("/%*.-%*/", "")
    local valueText = jsonText:match('"entityBits"%s*:%s*(%-?%d+)')
    if not valueText then
        IndexEntityBits = default
        return IndexEntityBits
    end

    local bits = tonumber(valueText)
    if bits == 16 or bits == 20 or bits == 22 or bits == 24 or bits == 28 then
        IndexEntityBits = bits
    else
        print("[Index] index-project.json: entityBits=" .. tostring(valueText)
            .. " is not one of {16, 20, 22, 24, 28}; falling back to 20.")
        IndexEntityBits = default
    end
    return IndexEntityBits
end

-- Writes the generated header that the EnTT patch reads to override
-- INDEX_ENTITY_BITS at compile time. The build-time -D in
-- GetIndexModuleDefines() (and in package-loader.lua's RegisterNative-
-- Project) is the fallback; the header is the override path that the
-- editor's Rebuild Engine flow uses to change the value without
-- regenerating project files.
--
-- Layout:
--   Index-Engine/src/Generated/IndexEntityBitsConfig.h
--
-- Every C++ project that compiles entt-using code has this folder on its
-- include path (the four main projects + RegisterNativeProject). On
-- premake regen we always rewrite the header so its value tracks
-- index-project.json. If the file ever drifts from the -D, the header
-- wins (it #undef/#defines INDEX_ENTITY_BITS).
IndexEntityBitsConfigHeader = path.join(ROOT_DIR,
    "Index-Engine/src/Generated/IndexEntityBitsConfig.h")
IndexEntityBitsConfigIncludeDir = path.join(ROOT_DIR,
    "Index-Engine/src/Generated")

function WriteIndexEntityBitsConfigHeader()
    local bits = GetIndexEntityBits()
    os.mkdir(IndexEntityBitsConfigIncludeDir)
    local file, err = io.open(IndexEntityBitsConfigHeader, "wb")
    if not file then
        error("[Index] cannot write " .. IndexEntityBitsConfigHeader
            .. " (" .. tostring(err) .. ")")
    end
    file:write(
        "// Auto-generated by premake (and rewritten by the editor's\n" ..
        "// Rebuild Engine flow). Do not edit by hand. Overrides the\n" ..
        "// build-time -DINDEX_ENTITY_BITS so the editor can change the\n" ..
        "// EnTT entity/version bit split without a full premake regen.\n" ..
        "// See premake5.lua's WriteIndexEntityBitsConfigHeader() and\n" ..
        "// Index-Editor/src/Systems/ImGuiEditorLayerPanels.cpp's\n" ..
        "// Rebuild Engine handler for the two writers.\n" ..
        "#pragma once\n" ..
        "#undef  INDEX_ENTITY_BITS\n" ..
        "#define INDEX_ENTITY_BITS " .. tostring(bits) .. "\n"
    )
    file:close()
end

-- Run once at premake-time so the header exists before any project block
-- is processed. The editor's Rebuild Engine path rewrites this same file
-- from C++ when only the bit count changes (avoiding the premake regen).
WriteIndexEntityBitsConfigHeader()

function GetIndexModuleDefines()
    local hasApplication = IndexModules.Render
        and IndexModules.Audio
        and IndexModules.Physics
        and IndexModules.Scripting
        and IndexModules.Editor

    local defines =
    {
        "INDEX_WITH_RENDER=" .. (IndexModules.Render and "1" or "0"),
        "INDEX_WITH_AUDIO=" .. (IndexModules.Audio and "1" or "0"),
        "INDEX_WITH_PHYSICS=" .. (IndexModules.Physics and "1" or "0"),
        "INDEX_WITH_SCRIPTING=" .. (IndexModules.Scripting and "1" or "0"),
        "INDEX_WITH_EDITOR=" .. (IndexModules.Editor and "1" or "0"),
        "INDEX_WITH_APPLICATION=" .. (hasApplication and "1" or "0"),
        "INDEX_ENTITY_BITS=" .. tostring(GetIndexEntityBits()),
        -- Shrink magic_enum's default reflection window [-128, 127] -> [-1, 64].
        -- All engine-side reflected enums are small `enum class : uint8_t`
        -- with sequential values in [0, ~16] (see Index-Engine/src/Components/
        -- and Index-Engine/src/Graphics/Wrap.hpp for the only customized
        -- range). The default window instantiates 256 template specializations
        -- per reflected enum; ComponentInspectors.cpp reflects so many enums
        -- via Properties::MakeWith<T>() that even 64-bit cl.exe runs out of
        -- heap (C1060 in magic_enum.hpp). Shrinking the window cuts template
        -- bloat ~4x. If a new enum exceeds [-1, 64], specialize
        -- magic_enum::customize::enum_range<T> in its header (see
        -- Index-Engine/src/Graphics/Wrap.hpp for the existing pattern).
        "MAGIC_ENUM_RANGE_MIN=-1",
        "MAGIC_ENUM_RANGE_MAX=32"
    }

    if IndexModules.FullCompatibility then
        table.insert(defines, "INDEX_ALL_MODULES=1")
    end

    return defines
end

function UseIndexEngineModuleDependencies()
    UseDependencySet(Dependency.EngineSelectedModules)
end

-- Shared postbuild command: copy IndexAssets into each target output
-- directory. The single source tree is `Index-Runtime/IndexAssets/`,
-- staged via one {COPYDIR} next to the consumer exe.
CopyIndexAssets = '{COPYDIR} "' .. path.join(ROOT_DIR, "Index-Runtime/IndexAssets") .. '" "%{cfg.targetdir}/IndexAssets"'

-- Shared postbuild command: copy the engine SharedLib next to each consumer executable
-- so it resolves at runtime without depending on PATH.
CopyIndexEngineDll = '{COPYFILE} "' ..
    path.join(ROOT_DIR, "bin/" .. outputdir, "Index-Engine", "Index-Engine.dll") ..
    '" "%{cfg.targetdir}/Index-Engine.dll"'

-- The Index-GameComponents sidecar DLL is removed — user components now
-- register at runtime via DynamicComponentRegistrar in Index-ScriptCore.
-- No per-project codegen artifact gets shipped alongside consumer exes.
CopyIndexGameComponentsDll = "" -- retained as a no-op so any leftover postbuild reference stays harmless

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
if IndexProfiler and IndexProfiler.Enabled then
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
-- own premake working directory. Top-level project folders (Index-Engine/,
-- Index-Editor/, etc.) need "../"; deeper folders (Tests/Index-Engine-
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
if IndexModules.Editor then
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
            -- See Index-Engine/premake5.lua for the rationale on
            -- MultiProcessorCompile + /Zc:preprocessor.
            flags { "MultiProcessorCompile" }
            buildoptions { "/FS", "/Zc:preprocessor" }
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

if IndexModules.Render then
    include "premake/dependencies/glfw.lua"
    -- Dawn (WebGPU) is the engine's sole GPU backend. The pre-built
    -- webgpu_dawn.lib comes from scripts/dawn/SetupDawn.bat — premake just
    -- links against it; see dawn.lua / DawnLayout for the paths.
    -- (No project here, no premake-side compilation — Dawn is too large
    -- to mirror into premake.)
end

if IndexModules.Physics then
    include "premake/dependencies/box2d.lua"
    include "premake/dependencies/index_physics.lua"
end

if IndexProfiler.Enabled then
    include "premake/dependencies/tracy.lua"
end

include "Index-Engine"

if IndexModules.Scripting then
    include "Index-ScriptCore"
    include "Index-Sandbox"
end

if IndexModules.Editor then
    include "Index-Editor"
end

if IndexModules.FullCompatibility then
    include "Index-Launcher"
    include "Index-Runtime"
end

group ""

-- Tests live in their own group so they're easy to spot in the IDE solution
-- explorer and trivial to disable by commenting out this block.
group "Tests"
    include "Tests/Index-Engine-Tests"
group ""

-- Load any index-package.lua manifests under packages/ and register their projects.
local IndexPackageLoader = dofile(path.join(ROOT_DIR, "premake/package-loader.lua"))
IndexPackageLoader.LoadAll()
