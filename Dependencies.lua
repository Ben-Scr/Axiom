IncludeDir = {}
IncludeDir["ExternalRoot"] = "External"
IncludeDir["ImGui"] = "External/imgui"
IncludeDir["Spdlog"] = "External/spdlog/include"
IncludeDir["GLFW"] = "External/glfw/include"
IncludeDir["AxiomPhysics"] = "External/Axiom-Physics/include"
IncludeDir["Box2D"] = "External/box2d/include"
IncludeDir["GLM"] = "External/glm"
IncludeDir["EnTT"] = "External/entt/src"
IncludeDir["STB"] = "External/stb"
IncludeDir["MagicEnum"] = "External/magic_enum/include"
IncludeDir["MiniAudio"] = "External/miniaudio"
IncludeDir["Cereal"] = "External/cereal/include"
IncludeDir["Glad"] = "External/glad/include"
IncludeDir["DotNet"] = "External/dotnet"
IncludeDir["AxiomEngine"] = "Axiom-Engine/src"
IncludeDir["AxiomEngineLegacy"] = "Axiom-Engine/src"
IncludeDir["Tracy"] = "External/tracy/public"

local isWindowsTarget = os.target() == "windows"

local function AppendUnique(target, values)
    if not values then
        return
    end

    local seen = {}
    for _, value in ipairs(target) do
        seen[value] = true
    end

    for _, value in ipairs(values) do
        if not seen[value] then
            table.insert(target, value)
            seen[value] = true
        end
    end
end

-- Concatenate any number of list-style tables into a single new table,
-- dropping duplicates. Used by the per-RHI dep sets below to splice in
-- the active backend's include/lib/define lists without mutating the
-- inputs. Returns a fresh table — callers can keep mutating it freely.
function MergeListsForDep(...)
    local result = {}
    local seen = {}
    for i = 1, select("#", ...) do
        local list = select(i, ...)
        if list then
            for _, value in ipairs(list) do
                if not seen[value] then
                    table.insert(result, value)
                    seen[value] = true
                end
            end
        end
    end
    return result
end

function MergeDependencySets(...)
    local merged =
    {
        IncludeDirs = {},
        LibDirs = {},
        DependsOn = {},
        Links = {},
        Defines = {}
    }

    for index = 1, select("#", ...) do
        local dependency = select(index, ...)
        if dependency then
            AppendUnique(merged.IncludeDirs, dependency.IncludeDirs)
            AppendUnique(merged.LibDirs, dependency.LibDirs)
            AppendUnique(merged.DependsOn, dependency.DependsOn)
            AppendUnique(merged.Links, dependency.Links)
            AppendUnique(merged.Defines, dependency.Defines)
        end
    end

    return merged
end

local MergeDependencies = MergeDependencySets

Library = {}
Library["GLFW"] = "glfw3.lib"
Library["Box2D"] = "box2d.lib"
Library["FreeType"] = "freetype.lib"
Library["OpenGL"] = "%{cfg.system == 'windows' and 'opengl32.lib' or 'GL'}"
Library["GDI32"] = "gdi32.lib"
Library["NetHost"] = isWindowsTarget and "nethost.lib" or "nethost"

LibDir = {}
if isWindowsTarget then
    LibDir["DotNet"] = "External/dotnet/lib"
end

Dependency = {}
Dependency["ImGui"] =
{
    -- imgui_impl_glfw + imgui_impl_opengl3 serve the consumer executables
    -- (editor / launcher / runtime). imgui_impl_wgpu serves engine.dll
    -- (Gui/ImGuiImpl.cpp wraps it). Dawn's include paths splice in so
    -- imgui_impl_wgpu.cpp can find <webgpu/webgpu.h>; the WGPU_SHARED_-
    -- LIBRARY=0 define carries through too so every TU that touches the
    -- WebGPU C ABI agrees on linkage.
    IncludeDirs = MergeListsForDep(
        { "%{IncludeDir.ImGui}", "%{IncludeDir.ImGui}/backends", "%{IncludeDir.GLFW}" },
        DawnLayout and DawnLayout.IncludeDirs or {}
    ),
    Defines = DawnLayout and DawnLayout.Defines or {},
    BuildProject = true
}

Dependency["Spdlog"] =
{
    IncludeDirs = { "%{IncludeDir.Spdlog}" },
    HeaderOnly = true
}

Dependency["ExternalIncludes"] =
{
    IncludeDirs =
    {
        "%{IncludeDir.GLFW}",
        "%{IncludeDir.Box2D}",
        "%{IncludeDir.GLM}",
        "%{IncludeDir.EnTT}",
        "%{IncludeDir.STB}",
        "%{IncludeDir.MagicEnum}",
        "%{IncludeDir.MiniAudio}",
        "%{IncludeDir.Cereal}",
        "%{IncludeDir.AxiomPhysics}"
    }
}

-- Minimal public core contract:
-- - Axiom-Engine/src headers
-- - Core/Export.hpp feature metadata
-- - Axiom.hpp lean umbrella
-- - header-only/public dependencies required by that core surface
Dependency["EngineCore"] =
{
    IncludeDirs =
    {
        "%{IncludeDir.AxiomEngine}",
        "%{IncludeDir.AxiomEngineLegacy}",
        "%{IncludeDir.Spdlog}",
        "%{IncludeDir.GLM}",
        "%{IncludeDir.EnTT}",
        "%{IncludeDir.STB}",
        "%{IncludeDir.MagicEnum}",
        -- Cereal: the wrapper Serialization/Cereal.hpp was removed (audit E5)
        -- because it had zero callers. The include path stays available for
        -- Packages/CsprojParser.cpp which still needs cereal/external/rapidxml
        -- for .csproj XML parsing. We tried scoping this with a per-file
        -- premake filter but it doesn't reliably emit per-file
        -- AdditionalIncludeDirectories on vcxproj — keeping it global is the
        -- pragmatic fix; it's a one-line cost for a header path that only
        -- one .cpp actually consumes.
        "%{IncludeDir.Cereal}"
    },

    LibDirs = {},

    DependsOn = {},

    Links = {}
}

-- Render-API dep set — WebGPU via Dawn. The legacy direct-OpenGL path was
-- retired on 2026-05; the bgfx interregnum that succeeded it was retired
-- on Stage 10 of the WebGPU port (bgfx submodules deleted from External/,
-- per-RHI dep splits collapsed into this single entry). Dawn is built
-- out-of-band by scripts/SetupDawn.bat into a monolithic webgpu_dawn.lib;
-- premake/dependencies/dawn.lua exposes DawnLayout with the resulting
-- include + lib paths. No DependsOn entry for a premake project because
-- Dawn isn't built by premake — it's a vendored pre-built static lib.
Dependency["EngineCoreRender"] =
{
    IncludeDirs = MergeListsForDep(
        { "%{IncludeDir.GLFW}" },
        DawnLayout and DawnLayout.IncludeDirs or {}
    ),

    -- Dawn's webgpu_dawn.lib lives under either Debug\ or Release\, and
    -- mixing runtime libraries (/MDd Debug vs /MD Release) trips MSVC's
    -- "_ITERATOR_DEBUG_LEVEL" + "RuntimeLibrary" mismatch (LNK2038). So
    -- LibDirs MUST be set per-config, not as a single default. The
    -- per-config wiring lives in Axiom-Engine/premake5.lua via explicit
    -- `filter "configurations:Debug" / Release / Dist` blocks that
    -- libdirs() each into the matching Dawn output folder.
    LibDirs = {},

    Defines = MergeListsForDep(
        { "GLFW_DLL", "AIM_RHI_WEBGPU" },
        DawnLayout and DawnLayout.Defines or {}
    ),

    DependsOn = { "GLFW" },

    Links = MergeListsForDep(
        { "GLFW" },
        DawnLayout and DawnLayout.Links or {}
    )
}

Dependency["EngineCoreAudio"] =
{
    IncludeDirs =
    {
        "%{IncludeDir.MiniAudio}"
    },

    LibDirs = {},

    DependsOn = {},

    Links = {}
}

Dependency["EngineCorePhysics"] =
{
    IncludeDirs =
    {
        "%{IncludeDir.Box2D}",
        "%{IncludeDir.AxiomPhysics}"
    },

    LibDirs = {},

    DependsOn =
    {
        "Box2D",
        "Axiom-Physics"
    },

    Links =
    {
        "Box2D",
        "Axiom-Physics"
    }
}

Dependency["EngineCoreScripting"] =
{
    IncludeDirs =
    {
        "%{IncludeDir.DotNet}"
    },

    LibDirs = {},

    DependsOn = {},

    Links =
    {
        "%{Library.NetHost}"
    }
}

Dependency["EngineCoreEditor"] =
{
    IncludeDirs =
    {
        "%{IncludeDir.ImGui}",
        "%{IncludeDir.ImGui}/backends"
    },

    LibDirs = {},

    DependsOn =
    {
        "ImGui"
    },

    Links =
    {
        "ImGui"
    }
}

-- Tracy client — populated only when AxiomProfiler.Enabled. Engine.dll +
-- editor.exe + runtime.exe all attach the same Tracy client SharedLib so
-- there's exactly one client instance per process. Header `Tracy.hpp`
-- sees TRACY_IMPORTS on consumer side -> dllimport, matching the
-- TRACY_EXPORTS in the Tracy project itself.
if AxiomProfiler and AxiomProfiler.Enabled then
    Dependency["Profiler"] =
    {
        IncludeDirs = { "%{IncludeDir.Tracy}" },
        DependsOn   = { "Tracy" },
        Links       = { "Tracy" },
        -- TRACY_ON_DEMAND must match the Tracy lib build (premake/dependencies/tracy.lua).
        -- Mismatched on/off across consumer + lib produces ABI drift in the SourceLocationData
        -- struct, which is hashed by Tracy at runtime — wrong size = corrupted zones.
        Defines     = { "AXIOM_PROFILER_ENABLED", "TRACY_ENABLE", "TRACY_IMPORTS", "TRACY_ON_DEMAND" }
    }
else
    Dependency["Profiler"] = { IncludeDirs = {}, DependsOn = {}, Links = {}, Defines = {} }
end

Dependency["EngineCoreAllModules"] = MergeDependencies(
    Dependency["EngineCore"],
    Dependency["EngineCoreRender"],
    Dependency["EngineCoreAudio"],
    Dependency["EngineCorePhysics"],
    Dependency["EngineCoreScripting"],
    Dependency["EngineCoreEditor"],
    Dependency["Profiler"]
)

-- Explicit legacy/full-module compatibility path for consumers that opt into AXIOM_ALL_MODULES.
Dependency["EngineCoreLegacy"] = Dependency["EngineCoreAllModules"]

Dependency["EditorRuntimeCommon"] = MergeDependencies(
    {
        DependsOn = { "Axiom-Engine" },
        Links = { "Axiom-Engine" }
    },
    Dependency["EngineCoreAllModules"]
)

if LibDir["DotNet"] then
    table.insert(Dependency["EngineCoreScripting"].LibDirs, "%{LibDir.DotNet}")
    table.insert(Dependency["EngineCoreAllModules"].LibDirs, "%{LibDir.DotNet}")
    table.insert(Dependency["EditorRuntimeCommon"].LibDirs, "%{LibDir.DotNet}")
end
