group "Game"
project "Index-Sandbox"
    location "."
    kind "SharedLib"
    language "C#"
    dotnetframework "net9.0"
    clr "Unsafe"

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    vsprops {
        AppendTargetFrameworkToOutputPath = "false",
        Nullable = "enable",
        AllowUnsafeBlocks = "true",
        EnableDynamicLoading = "true",
    }

    files {
        "Source/**.cs"
    }

    links {
        "Index-ScriptCore",
        -- Engine packages the Sandbox preset opts into. Removing entries here
        -- is the same as not installing the package — `Index.Hardware.Cpu`
        -- etc. simply won't resolve. Per the engine's "no mandatory
        -- dependencies" rule, every entry below is removable.
        "Pkg.Index.Hardware",
    }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        optimize "Off"
        symbols "On"
        defines { "IDX_DEBUG" }

    filter "configurations:Release"
        optimize "On"
        symbols "On"
        defines { "IDX_RELEASE" }

    filter "configurations:Dist"
        optimize "Full"
        symbols "Off"
        defines { "IDX_DIST" }
