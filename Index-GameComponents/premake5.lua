-- Index-GameComponents: the per-project native sidecar that owns C++
-- mirrors of user C# components marked [NativeComponent(..., Generate = true)].
--
-- The engine no longer compiles user component code into its own binary.
-- Instead this sidecar DLL exports `IndexRegisterCodegenComponents`,
-- which Index-Engine's CodegenSidecarLoader resolves with LoadLibrary +
-- GetProcAddress (dlopen/dlsym on Linux) when SceneManager registers its
-- built-in components. If the DLL is missing or the symbol can't be
-- resolved, the engine logs a warning and continues — equivalent to the
-- old "no user components" no-op stub.
--
-- The single TU at src/CodegenComponents.cpp is overwritten by
-- Index-ComponentCodegen on every editor "Rebuild Engine" run. The
-- file on disk at fresh-checkout time is a stub that exports the same
-- symbol with an empty body so this project always builds.
group "Game"
project "Index-GameComponents"
    location "."
    kind "SharedLib"
    language "C++"
    cppdialect "C++20"
    cdialect "C17"
    staticruntime "off"
    warnings "Extra"

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "src/**.h",
        "src/**.hpp",
        "src/**.cpp"
    }

    -- The sidecar consumes Index-Engine's public headers + INDEX_API
    -- exports. IDX_IMPORT_DLL flips INDEX_API to __declspec(dllimport)
    -- so the link resolves at runtime against engine.dll.
    UseDependencySet({
        DependsOn = { "Index-Engine" },
        Links     = { "Index-Engine" }
    })
    UseIndexEngineModuleDependencies()
    defines(GetIndexModuleDefines())
    defines { "IDX_IMPORT_DLL" }

    -- Generated/IndexEntityBitsConfig.h is read by the EnTT patch; any
    -- TU that includes entt (transitively via pch.hpp) needs it on the
    -- include path. See WriteIndexEntityBitsConfigHeader() in the root
    -- premake5.lua.
    includedirs { IndexEntityBitsConfigIncludeDir }

    filter "system:windows"
        flags { "MultiProcessorCompile" }
        buildoptions { "/utf-8", "/FS", "/Zc:preprocessor" }
        systemversion "latest"
        defines { "IDX_PLATFORM_WINDOWS" }

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

    filter {}

    -- Per-config libdirs for webgpu_dawn.lib (inherited via
    -- EngineCoreRender's Links). Same LNK2038 rationale as the consumer
    -- exes — see ApplyDawnLibDirs in the root premake5.lua.
    ApplyDawnLibDirs("../")
