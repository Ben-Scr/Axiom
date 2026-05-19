group "Runtime"
project "Index-Editor"
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
    files { path.join(ROOT_DIR, "Index-Engine/src/Diagnostics/**.cpp") }

    -- The ImGui WebGPU backend lives inside Index-Engine.dll so the
    -- editor and engine share one wgpu::Device. The editor's
    -- ImGuiContextLayer.cpp `#includes "Gui/ImGuiImplWebGPU.hpp"` via
    -- the engine include path and links the INDEX_API exports.

    UseDependencySet(Dependency.EditorRuntimeCommon)
    defines(GetIndexModuleDefines())
    defines { "IDX_IMPORT_DLL" }
    includedirs { "src" }
    -- The editor's Rebuild Engine flow rewrites IndexEntityBitsConfig.h
    -- to override INDEX_ENTITY_BITS without a premake regen. The EnTT
    -- patch picks it up via `#include "IndexEntityBitsConfig.h"`. See
    -- WriteIndexEntityBitsConfigHeader() in the root premake5.lua.
    includedirs { IndexEntityBitsConfigIncludeDir }

    -- Precompile the engine's pch.hpp once per editor build instead of
    -- re-parsing it in every TU. EditorPch.cpp is a one-line stub that just
    -- includes pch.hpp; the file lives in src/ so it's picked up by the
    -- "src/**.cpp" files glob above. /FI pch.hpp (forceincludes) auto-prepends
    -- the PCH header to TUs that don't already include it (e.g. the engine's
    -- Diagnostics .cpp files added below, and a handful of editor .cpp files
    -- that include their own header first instead of pch.hpp). Without this
    -- PCH setup ComponentInspectors.cpp exhausts the compiler heap with C1060
    -- even with /Zm2000 + 64-bit hosted cl.exe.
    pchheader "pch.hpp"
    pchsource "src/EditorPch.cpp"
    forceincludes { "pch.hpp" }
    postbuildcommands { CopyIndexAssets, CopyIndexEngineDll, CopyGlfwDll, CopyGladDll }
    if IndexModules.Scripting then postbuildcommands { CopyIndexGameComponentsDll } end
    if IndexProfiler.Enabled then postbuildcommands { CopyTracyDll } end

    filter "system:windows"
        -- See Index-Engine/premake5.lua for the rationale on
        -- MultiProcessorCompile + /Zc:preprocessor.
        --
        -- /Zm2000 raises the MSVC compiler's heap reservation to the maximum.
        -- Required for ComponentInspectors.cpp's Properties::Make<EnumType>/
        -- MakeWith<T> template forest. Even after tightening
        -- MAGIC_ENUM_RANGE_MIN/MAX in the workspace defines and switching
        -- to the 64-bit hosted cl.exe (Directory.Build.props sets
        -- PreferredToolArchitecture=x64), the editor's lack of a real PCH
        -- means every TU reparses Index-Engine/src/pch.hpp's full STL +
        -- magic_enum + ImGui surface area, exhausting compiler heap (C1060).
        flags { "MultiProcessorCompile" }
        buildoptions { "/utf-8", "/FS", "/Zm2000", "/Zc:preprocessor" }
        systemversion "latest"
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

    -- ComponentInspectors.cpp and EditorComponentRegistration.cpp instantiate
    -- the same kind of heavy Properties::Make<EnumType>/MakeWith<T>/MakeVariant
    -- template tree per registered component as Index-Engine's
    -- BuiltInComponentRegistration.cpp (which already has /bigobj — see
    -- Index-Engine/premake5.lua's "files:**/BuiltInComponentRegistration.cpp"
    -- filter). Without /bigobj these TUs hit the 64K COFF section limit
    -- (C1128) once a handful more components are added.
    filter "files:**/ComponentInspectors.cpp"
        buildoptions { "/bigobj" }

    filter "files:**/EditorComponentRegistration.cpp"
        buildoptions { "/bigobj" }

    filter {}
