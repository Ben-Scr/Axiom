project "Glad"
    location (path.join(ROOT_DIR, "premake/generated/Glad"))
    -- SharedLib for the same reason GLFW is: avoid duplicating Glad's global function
    -- pointers between engine.dll and each consumer executable.
    kind "SharedLib"
    language "C"
    cdialect "C17"
    staticruntime "off"

    -- GLAD_GLAPI_EXPORT + GLAD_GLAPI_EXPORT_BUILD flip GLAPI to __declspec(dllexport);
    -- consumers define GLAD_GLAPI_EXPORT only.
    defines { "GLAD_GLAPI_EXPORT", "GLAD_GLAPI_EXPORT_BUILD" }

    targetdir (path.join(ROOT_DIR, "bin/" .. outputdir .. "/%{prj.name}"))
    objdir (path.join(ROOT_DIR, "bin-int/" .. outputdir .. "/%{prj.name}"))

    files
    {
        path.join(ROOT_DIR, "External/glad/include/**.h"),
        path.join(ROOT_DIR, "External/glad/src/**.c")
    }

    includedirs
    {
        path.join(ROOT_DIR, "External/glad/include")
    }

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
