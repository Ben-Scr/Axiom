-- bgfx + dependencies (bx + bimg).
--
-- Stage 1 of the bgfx port (see Graphics/RenderApi.hpp for context). These
-- three projects are added when premake is invoked with --rhi=bgfx so the
-- engine's BgfxApi backend has something to link against. The OpenGL build
-- (--rhi=opengl, the default) skips them entirely so existing users don't
-- pay the build cost.
--
-- All three libs use bgfx's own "amalgamated.cpp" single-TU build pattern —
-- one .cpp per project pulls in everything else via #include. That keeps the
-- premake file short and avoids the need to mirror bgfx's per-platform file
-- selection logic from its genie scripts. The amalgamated TU is a maintained,
-- supported entry point.
--
-- Static libs (matches Box2D's pattern), linked into Axiom-Engine.dll. bgfx
-- has its own SharedLib build option upstream but the single-DLL Axiom story
-- stays simpler with everything statically linked into the engine.

-- ── bx ────────────────────────────────────────────────────────────
project "bx"
    location (path.join(ROOT_DIR, "premake/generated/bx"))
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    exceptionhandling "Off"
    rtti "Off"

    targetdir (path.join(ROOT_DIR, "bin/" .. outputdir .. "/%{prj.name}"))
    objdir (path.join(ROOT_DIR, "bin-int/" .. outputdir .. "/%{prj.name}"))

    files
    {
        path.join(ROOT_DIR, "External/bx/src/amalgamated.cpp"),
        path.join(ROOT_DIR, "External/bx/include/**.h"),
        path.join(ROOT_DIR, "External/bx/include/**.inl"),
    }

    includedirs
    {
        path.join(ROOT_DIR, "External/bx/include"),
        path.join(ROOT_DIR, "External/bx/3rdparty"),
    }

    -- Defines required by bx for any TU including its headers. Documented
    -- in bx's own genie script (scripts/bx.lua) and in tinystl/limits.h.
    defines
    {
        "__STDC_LIMIT_MACROS",
        "__STDC_FORMAT_MACROS",
        "__STDC_CONSTANT_MACROS",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/FS", "/Zc:__cplusplus", "/Zc:preprocessor" }
        defines { "_CRT_SECURE_NO_WARNINGS", "_HAS_EXCEPTIONS=0" }
        -- bx ships per-toolchain compatibility shims (e.g. an alloca.h
        -- replacement for MSVC). Without this dir, amalgamated.cpp picks
        -- up the wrong compat headers on MSVC.
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/msvc") }

    filter "system:linux"
        pic "On"
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/linux") }

    filter "system:macosx"
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/osx") }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"
        defines { "BX_CONFIG_DEBUG=1" }

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
        symbols "On"
        defines { "BX_CONFIG_DEBUG=0" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "Full"
        symbols "Off"
        defines { "BX_CONFIG_DEBUG=0" }

    filter {}


-- ── bimg ──────────────────────────────────────────────────────────
-- Image loader used by bgfx for runtime texture decode. bgfx's amalgamated
-- build expects bimg to be available at link time — we don't strictly
-- need decode features for Stage 1 (engine has its own stb_image path),
-- but bgfx symbols expect to be resolved.
project "bimg"
    location (path.join(ROOT_DIR, "premake/generated/bimg"))
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    exceptionhandling "Off"
    rtti "Off"

    targetdir (path.join(ROOT_DIR, "bin/" .. outputdir .. "/%{prj.name}"))
    objdir (path.join(ROOT_DIR, "bin-int/" .. outputdir .. "/%{prj.name}"))

    -- bimg has no `amalgamated.cpp` shipped — its image.cpp is the entry
    -- TU but it doesn't include the rest. Walk the src/ tree explicitly.
    -- The astc encoder + nvtt + iqa + libsquish 3rdparty libs are needed
    -- for bimg_encode (which we don't link), so we only pull in the
    -- `image*.cpp` runtime files.
    files
    {
        path.join(ROOT_DIR, "External/bimg/include/**.h"),
        path.join(ROOT_DIR, "External/bimg/src/image.cpp"),
        path.join(ROOT_DIR, "External/bimg/src/image_gnf.cpp"),
        path.join(ROOT_DIR, "External/bimg/src/bimg_p.h"),
        path.join(ROOT_DIR, "External/bimg/src/config.h"),
        -- Decode + encode pulled in for completeness so bgfx's calls into
        -- bimg link cleanly. The encode path drags in astc/nvtt; both are
        -- header-only-ish from bgfx's perspective, sized as part of the
        -- bimg static lib.
        path.join(ROOT_DIR, "External/bimg/src/image_decode.cpp"),
        path.join(ROOT_DIR, "External/bimg/src/image_encode.cpp"),
        path.join(ROOT_DIR, "External/bimg/3rdparty/astc-encoder/source/**.cpp"),
        path.join(ROOT_DIR, "External/bimg/3rdparty/astc-encoder/source/**.h"),
    }

    includedirs
    {
        path.join(ROOT_DIR, "External/bimg/include"),
        path.join(ROOT_DIR, "External/bimg/3rdparty"),
        path.join(ROOT_DIR, "External/bimg/3rdparty/astc-encoder/include"),
        path.join(ROOT_DIR, "External/bimg/3rdparty/iqa/include"),
        -- image_decode.cpp does `#include <miniz/miniz.c>` — the miniz
        -- folder lives under tinyexr/deps in bimg's tree.
        path.join(ROOT_DIR, "External/bimg/3rdparty/tinyexr/deps"),
        path.join(ROOT_DIR, "External/bx/include"),
        path.join(ROOT_DIR, "External/bx/3rdparty"),
    }

    defines
    {
        "__STDC_LIMIT_MACROS",
        "__STDC_FORMAT_MACROS",
        "__STDC_CONSTANT_MACROS",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/FS", "/Zc:__cplusplus", "/Zc:preprocessor", "/wd4530", "/wd4324" }
        defines { "_CRT_SECURE_NO_WARNINGS", "_HAS_EXCEPTIONS=0" }
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/msvc") }

    filter "system:linux"
        pic "On"
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/linux") }

    filter "system:macosx"
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/osx") }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"
        defines { "BX_CONFIG_DEBUG=1" }

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
        symbols "On"
        defines { "BX_CONFIG_DEBUG=0" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "Full"
        symbols "Off"
        defines { "BX_CONFIG_DEBUG=0" }

    filter {}


-- ── bgfx ──────────────────────────────────────────────────────────
project "bgfx"
    location (path.join(ROOT_DIR, "premake/generated/bgfx"))
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    exceptionhandling "Off"
    rtti "Off"

    targetdir (path.join(ROOT_DIR, "bin/" .. outputdir .. "/%{prj.name}"))
    objdir (path.join(ROOT_DIR, "bin-int/" .. outputdir .. "/%{prj.name}"))

    files
    {
        path.join(ROOT_DIR, "External/bgfx/src/amalgamated.cpp"),
        path.join(ROOT_DIR, "External/bgfx/include/**.h"),
        path.join(ROOT_DIR, "External/bgfx/src/**.h"),
    }

    -- bgfx is selective about which renderers it includes. The amalgamated
    -- TU pulls in EVERY renderer file, but each renderer is internally gated
    -- by a `BGFX_CONFIG_RENDERER_*` define. By default bgfx's config.h
    -- enables a sane per-platform set (Direct3D11+12+OpenGL+Vulkan on
    -- Windows; Metal on macOS/iOS; OpenGLES on Android). We don't override
    -- any of that here — Stage 1 just needs *something* to clear the screen
    -- on each platform; backend selection at runtime will follow in a later
    -- sub-stage.

    includedirs
    {
        path.join(ROOT_DIR, "External/bgfx/include"),
        path.join(ROOT_DIR, "External/bgfx/src"),
        path.join(ROOT_DIR, "External/bgfx/3rdparty"),
        path.join(ROOT_DIR, "External/bgfx/3rdparty/khronos"),
        path.join(ROOT_DIR, "External/bgfx/3rdparty/dxsdk/include"),
        path.join(ROOT_DIR, "External/bgfx/3rdparty/directx-headers/include/directx"),
        path.join(ROOT_DIR, "External/bx/include"),
        path.join(ROOT_DIR, "External/bx/3rdparty"),
        path.join(ROOT_DIR, "External/bimg/include"),
    }

    defines
    {
        "__STDC_LIMIT_MACROS",
        "__STDC_FORMAT_MACROS",
        "__STDC_CONSTANT_MACROS",
        "BGFX_CONFIG_MULTITHREADED=0",  -- Single-threaded for Stage 1; matches the engine's existing render thread model.
    }

    links { "bx", "bimg" }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/FS", "/Zc:__cplusplus", "/Zc:preprocessor" }
        defines
        {
            "_CRT_SECURE_NO_WARNINGS",
            "_HAS_EXCEPTIONS=0",
            "NOMINMAX",
        }
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/msvc") }
        -- gdi32 + psapi pulled in by bgfx's win32 paths (window query +
        -- module enumeration). dxgi + d3d11 are loaded via LoadLibrary at
        -- runtime so they don't need static linking; including them here
        -- as a comment for the next-sub-stage author.
        links { "gdi32", "psapi" }

    filter "system:linux"
        pic "On"
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/linux") }
        links { "X11", "GL", "pthread", "dl" }

    filter "system:macosx"
        includedirs { path.join(ROOT_DIR, "External/bx/include/compat/osx") }
        links { "Cocoa.framework", "Metal.framework", "QuartzCore.framework" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"
        defines { "BX_CONFIG_DEBUG=1", "BGFX_CONFIG_DEBUG=1" }

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
        symbols "On"
        defines { "BX_CONFIG_DEBUG=0" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "Full"
        symbols "Off"
        defines { "BX_CONFIG_DEBUG=0" }

    filter {}
