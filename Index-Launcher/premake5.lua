group "Runtime"
project "Index-Launcher"
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

    UseDependencySet(Dependency.EditorRuntimeCommon)
    defines(GetIndexModuleDefines())
    defines { "IDX_IMPORT_DLL" }
    includedirs { "src" }
    -- See WriteIndexEntityBitsConfigHeader() in the root premake5.lua.
    includedirs { IndexEntityBitsConfigIncludeDir }

    -- The ImGui WebGPU backend lives inside Index-Engine.dll so the
    -- launcher and engine share one wgpu::Device. The launcher just
    -- `#includes` the engine header which exports the API via
    -- INDEX_API.

    postbuildcommands { CopyIndexAssets, CopyIndexEngineDll, CopyGlfwDll, CopyGladDll }
    if IndexProfiler.Enabled then postbuildcommands { CopyTracyDll } end

    filter "system:windows"
        -- See Index-Engine/premake5.lua for the rationale on
        -- MultiProcessorCompile + /Zc:preprocessor.
        flags { "MultiProcessorCompile" }
        buildoptions { "/utf-8", "/FS", "/Zc:preprocessor" }
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
