-- Dawn (Google's WebGPU implementation) wiring.
--
-- Stage 1 of the WebGPU port (see Axiom-Engine/src/Graphics/Backend/WebGPUApi.cpp
-- for context). Dawn is built out-of-band by scripts/SetupDawn.bat or
-- scripts/SetupDawn.sh as a monolithic static library — premake does NOT compile
-- Dawn here. Dawn has hundreds of source files with CMake-driven codegen
-- (.h files emitted from .json descriptions of the WebGPU API surface); trying
-- to mirror that into premake would be a porting tax we'd pay every Dawn
-- update. Building Dawn via its own CMake once, then linking the resulting
-- webgpu_dawn.lib, is the supported pattern.
--
-- This file is included from premake5.lua when --rhi=webgpu is in effect.
-- It is intentionally NOT a premake `project` — there's nothing here for
-- premake to build. We instead export a small `DawnLayout` table the rest
-- of the build script (Dependencies.lua / Axiom-Engine/premake5.lua) reads
-- to inject Dawn's include + lib paths into the engine project.
--
-- If SetupDawn hasn't been run yet, the engine link step will fail with
-- "cannot open input file 'webgpu_dawn.lib'" — that's the trigger to run
-- SetupDawn.bat. We could fail earlier in premake itself by checking
-- os.isfile here, but a premake-time check is fragile across reconfigure-
-- before-build sequencing; the link-time failure message is clearer.

DawnLayout = {}

-- Public header tree (the standard webgpu/ headers — webgpu.h, webgpu_cpp.h).
DawnLayout.IncludeDirs =
{
    -- Public webgpu-headers spec — webgpu.h + webgpu_cpp.h ship under
    -- include/dawn/webgpu/ in newer Dawn revisions and include/webgpu/
    -- in older ones. We add both so #include <webgpu/webgpu_cpp.h>
    -- resolves regardless of which checkout the developer has.
    "External/dawn/include",
    "External/dawn/include/dawn",
    -- Generated headers (wgpu_string_view.h, webgpu_enum_class_bitmasks.h,
    -- etc.) live in the CMake build directory.
    "External/dawn/build/gen/include",
}

-- Per-config lib paths. The monolithic webgpu_dawn target lands in
-- src/dawn/native/<Config>/ for the multi-config Visual Studio generator,
-- and src/dawn/native/ for single-config (Make / Ninja). We list both so
-- a single libdirs() call covers either case — the unused path is a
-- harmless miss at link time.
DawnLayout.LibDirsDebug =
{
    "External/dawn/build/src/dawn/native/Debug",
    "External/dawn/build/src/dawn/native",
}
DawnLayout.LibDirsRelease =
{
    "External/dawn/build/src/dawn/native/Release",
    "External/dawn/build/src/dawn/native",
}

-- A single monolithic lib -- built with -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC
-- so we don't have to enumerate the ~40 internal Dawn/Tint sub-libs and
-- their shifting names. If a future Dawn version renames this, update here
-- in one place.
--
-- The extra Windows SDK libs cover Dawn's D3D11/D3D12 backends:
--   * dxguid.lib  -- WKPDID_D3DDebugObjectName (the debug-name marker
--     constant Dawn's SetDebugName helper attaches to D3D resources).
--   * onecore.lib -- CompareObjectHandles (Win 8.1+ kernel API Dawn uses
--     to compare D3D shared-fence handles in SharedFenceD3D11/12.cpp).
-- Both are header-only declarations; the actual symbols ship with the
-- Windows SDK and aren't auto-linked by default.
DawnLayout.Links =
{
    "webgpu_dawn",
    "dxguid",
    "onecore",
}

-- IMPORTANT: do NOT define WGPU_SHARED_LIBRARY here. The webgpu.h header
-- uses `#if defined(WGPU_SHARED_LIBRARY)` to decide whether to emit
-- __declspec(dllimport) on every WebGPU function. `#if defined` is
-- value-agnostic — defining it as `0` is still "defined" and trips the
-- dllimport path, which produces `__imp_wgpuCreateInstance` link errors
-- against the static webgpu_dawn.lib. Leaving it UNDEFINED is the correct
-- static-link configuration (WGPU_EXPORT collapses to nothing).
DawnLayout.Defines =
{
    -- The C++ wrapper's enum-class operators are gated behind this flag in
    -- recent Dawn versions. Without it, bitwise ops on wgpu::TextureUsage
    -- (and similar enum classes) silently degrade to integer math and
    -- decay-to-int warnings flood the build.
    "WEBGPU_CPP_HAS_ENUM_CLASS_BITMASKS=1",
    -- imgui_impl_wgpu.cpp (and any TU that includes imgui_impl_wgpu.h)
    -- now requires an explicit backend selector. We use Dawn, so flag it.
    -- Without this define both the ImGui static lib's compilation of
    -- imgui_impl_wgpu.cpp AND engine.dll's ImGuiImplWebGPU.cpp fail with
    -- `#error: Exactly one of IMGUI_IMPL_WEBGPU_BACKEND_{DAWN,WGPU,WGVK}
    -- must be defined`.
    "IMGUI_IMPL_WEBGPU_BACKEND_DAWN",
}
