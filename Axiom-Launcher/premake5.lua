group "Runtime"
project "Axiom-Launcher"
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
    defines(GetAxiomModuleDefines())
    defines { "AIM_IMPORT_DLL" }
    includedirs { "src" }

    -- ImGuiImplBgfx now lives inside Axiom-Engine.dll (was previously
    -- statically pulled into every consumer .exe). bgfx's renderer
    -- state is held in process-global statics; if multiple binaries
    -- in one process each static-link bgfx, each gets its own
    -- uninitialised copy. The launcher's bgfx state was reading as
    -- `RendererType::Noop` even though engine.dll's `bgfx::init` had
    -- already brought up D3D11 — `ImGuiImplBgfx::ResolveImguiBin`
    -- then hit the `default:` switch arm and returned an empty
    -- shader path. Letting engine.dll own the imgui-bgfx backend
    -- (same TU as `bgfx::init`) keeps both running against the same
    -- bgfx state. The launcher just `#includes` the engine header
    -- which exports the API via AXIOM_API.

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
