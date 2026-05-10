@echo off
:: Recompiles every .sc shader in this directory for D3D11 + GLSL + SPIRV.
:: Run from the repo root (parent of External/, Axiom-Runtime/, etc.).
::
:: Prerequisite: shadercRelease.exe must exist at the path below — it's
:: built by:
::   cd External\bgfx
::   ..\bx\tools\bin\windows\genie.exe --with-tools vs2022
::   msbuild .build\projects\vs2022\shaderc.vcxproj -p:Configuration=Release -p:Platform=x64
::
:: That run is a one-time setup; re-running this script after editing a
:: .sc file produces fresh .bin/<profile>/<stage>_<name>.bin outputs the
:: engine's Shader_Bgfx loader picks up at runtime.
::
:: Source location: the bgfx shader source + bin output now lives under
:: Axiom-Runtime/AxiomAssets/Shaders/ alongside the OpenGL Shader/ tree
:: so a single CopyAxiomAssets postbuild stages every render-backend's
:: assets next to the consumer .exe.

setlocal enabledelayedexpansion

set SHADERC=External\bgfx\.build\win64_vs2022\bin\shadercRelease.exe
set SRCDIR=Axiom-Runtime\AxiomAssets\Shaders\bgfx

if not exist "%SHADERC%" (
    echo [ERROR] shaderc not found at %SHADERC%
    echo See the header comment in this script for the one-time build command.
    exit /b 1
)

if not exist "%SRCDIR%\bin\dx11"  mkdir "%SRCDIR%\bin\dx11"
if not exist "%SRCDIR%\bin\glsl"  mkdir "%SRCDIR%\bin\glsl"
if not exist "%SRCDIR%\bin\spirv" mkdir "%SRCDIR%\bin\spirv"

:: Each shader name is paired with its own varying-def file because the
:: vertex inputs differ:
::   sprite : per-instance Pos+Scale, Color, cos+sin (rotation)
::   imgui  : per-vertex Pos+UV+RGBA8 (ImDrawVert)
::   text   : per-vertex Pos+UV+RGBA32 (TextVertex)
:: A single shared varying.def couldn't express all three without
:: pulling in attributes the wrong shader expects to read.
call :CompileShader sprite varying.def.sc
if errorlevel 1 exit /b 1
call :CompileShader imgui  imgui_varying.def.sc
if errorlevel 1 exit /b 1
call :CompileShader text   text_varying.def.sc
if errorlevel 1 exit /b 1
call :CompileShader gizmo  gizmo_varying.def.sc
if errorlevel 1 exit /b 1

echo.
echo Done. Outputs in %SRCDIR%\bin\
endlocal
exit /b 0

:CompileShader
set NAME=%~1
set VARYING=%~2
echo === %NAME% (varying=%VARYING%) ===

"%SHADERC%" -f "%SRCDIR%\vs_%NAME%.sc" -o "%SRCDIR%\bin\dx11\vs_%NAME%.bin" ^
    --platform windows --type vertex --profile s_5_0 ^
    --varyingdef "%SRCDIR%\%VARYING%" -i "External\bgfx\src"
if errorlevel 1 exit /b 1
"%SHADERC%" -f "%SRCDIR%\fs_%NAME%.sc" -o "%SRCDIR%\bin\dx11\fs_%NAME%.bin" ^
    --platform windows --type fragment --profile s_5_0 ^
    --varyingdef "%SRCDIR%\%VARYING%" -i "External\bgfx\src"
if errorlevel 1 exit /b 1

"%SHADERC%" -f "%SRCDIR%\vs_%NAME%.sc" -o "%SRCDIR%\bin\glsl\vs_%NAME%.bin" ^
    --platform linux --type vertex --profile 120 ^
    --varyingdef "%SRCDIR%\%VARYING%" -i "External\bgfx\src"
if errorlevel 1 exit /b 1
"%SHADERC%" -f "%SRCDIR%\fs_%NAME%.sc" -o "%SRCDIR%\bin\glsl\fs_%NAME%.bin" ^
    --platform linux --type fragment --profile 120 ^
    --varyingdef "%SRCDIR%\%VARYING%" -i "External\bgfx\src"
if errorlevel 1 exit /b 1

"%SHADERC%" -f "%SRCDIR%\vs_%NAME%.sc" -o "%SRCDIR%\bin\spirv\vs_%NAME%.bin" ^
    --platform linux --type vertex --profile spirv ^
    --varyingdef "%SRCDIR%\%VARYING%" -i "External\bgfx\src"
if errorlevel 1 exit /b 1
"%SHADERC%" -f "%SRCDIR%\fs_%NAME%.sc" -o "%SRCDIR%\bin\spirv\fs_%NAME%.bin" ^
    --platform linux --type fragment --profile spirv ^
    --varyingdef "%SRCDIR%\%VARYING%" -i "External\bgfx\src"
if errorlevel 1 exit /b 1

exit /b 0
