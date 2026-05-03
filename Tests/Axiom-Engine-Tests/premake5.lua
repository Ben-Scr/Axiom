project "Axiom-Engine-Tests"
    location "."
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    warnings "Extra"

    targetdir ("../../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../../bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "src/**.cpp",
        "src/**.hpp",
        "src/**.h"
    }

    -- Link the engine SharedLib so we can call into Axiom:: functions directly
    -- (e.g. ParseInstalledPackagesFromXml).
    UseDependencySet(Dependency.EditorRuntimeCommon)
    defines(GetAxiomModuleDefines())
    defines { "AIM_IMPORT_DLL" }

    -- doctest is a single-header lib at External/doctest/doctest/doctest.h.
    -- Include path "External/doctest" gives the conventional `#include <doctest/doctest.h>`.
    includedirs { path.join(ROOT_DIR, "External/doctest") }

    -- Same DLL-copying postbuild as Runtime so the test exe finds Axiom-Engine.dll
    -- next to it at run time.
    postbuildcommands
    {
        CopyAxiomEngineDll,
        CopyGlfwDll,
        CopyGladDll
    }
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
