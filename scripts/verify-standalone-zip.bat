@echo off
REM Verify that the Index-Launcher folder is shippable as a standalone zip.
REM Checks file presence directly under bin\<Config>-windows-x86_64\
REM Index-Launcher\ and inside the bundled EngineSDK so reach-backs into
REM the source tree are caught.
REM
REM This is a structural check only. The full clean-machine validation
REM must run on a separate Windows box with no source tree on disk, see
REM the plan's Verification section.
REM
REM Usage:
REM   scripts\verify-standalone-zip.bat            uses Release config
REM   scripts\verify-standalone-zip.bat Debug      override config

setlocal EnableDelayedExpansion

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "PLATFORM=windows-x86_64"
set "ROOT=%~dp0.."
pushd "%ROOT%" >nul

set "LAUNCHER=%ROOT%\bin\%CONFIG%-%PLATFORM%\Index-Launcher"
set "SDK=%LAUNCHER%\EngineSDK"

echo [verify-standalone-zip] target: %LAUNCHER%
echo.

set "FAIL=0"

REM ---- Required files directly under the launcher folder
call :RequireFile "%LAUNCHER%\Index-Launcher.exe"
call :RequireFile "%LAUNCHER%\Index-Engine.dll"
call :RequireFile "%LAUNCHER%\GLFW.dll"
call :RequireFile "%LAUNCHER%\nethost.dll"
call :RequireDir  "%LAUNCHER%\IndexAssets"
call :RequireDir  "%LAUNCHER%\IndexAssets\Fonts"
call :RequireDir  "%LAUNCHER%\IndexAssets\Shader"

REM ---- EngineSDK folder must exist with the expected layout
call :RequireDir  "%SDK%"
call :RequireDir  "%SDK%\Index-Engine\src"
call :RequireFile "%SDK%\Index-ScriptCore\Index-ScriptCore.csproj"
call :RequireFile "%SDK%\Index-ScriptCore\Index-ScriptCore.dll"
call :RequireFile "%SDK%\bin\%CONFIG%-%PLATFORM%\Index-Engine\Index-Engine.lib"

REM ---- External vendored deps that the NativeScript CMakeLists references
call :RequireDir "%SDK%\External\spdlog\include"
call :RequireDir "%SDK%\External\glm"
call :RequireDir "%SDK%\External\entt\src"
call :RequireDir "%SDK%\External\stb"
call :RequireDir "%SDK%\External\magic_enum\include"
call :RequireDir "%SDK%\External\cereal\include"
call :RequireDir "%SDK%\External\glfw\include"
call :RequireDir "%SDK%\External\glad\include"
call :RequireDir "%SDK%\External\miniaudio"
call :RequireDir "%SDK%\External\box2d\include"
call :RequireDir "%SDK%\External\Axiom-Physics\include"
call :RequireDir "%SDK%\External\dotnet"

REM ---- Sanity: warn when the dev source tree is reachable above the launcher.
REM     ResolveEngineRoot in IndexProject.cpp walks exeDir\..\..\.. first so a
REM     dev build keeps using the source tree; EngineSDK is the fallback. The
REM     warning reminds the operator that a true standalone test must run on
REM     a machine with no source tree on disk anywhere reachable from the exe.
if exist "%LAUNCHER%\..\..\..\Index-Engine\src" goto :devTreeNote
goto :devTreeDone

:devTreeNote
echo [verify-standalone-zip] NOTE: dev source tree is reachable three dirs above the launcher.
echo [verify-standalone-zip]       Launcher will resolve the dev tree first; EngineSDK is
echo [verify-standalone-zip]       the fallback. A real standalone test must use a machine
echo [verify-standalone-zip]       without the repo anywhere reachable from the unzipped exe.

:devTreeDone
echo.
if "%FAIL%"=="0" goto :ok
goto :bad

:ok
echo [verify-standalone-zip] OK: launcher folder structure looks shippable.
popd >nul
endlocal
exit /b 0

:bad
echo [verify-standalone-zip] FAIL: %FAIL% missing artifacts. Build %CONFIG% and re-run.
popd >nul
endlocal
exit /b 1

:RequireFile
if exist "%~1" exit /b 0
echo   MISSING file:   %~1
set /a FAIL+=1
exit /b 0

:RequireDir
if exist "%~1\" exit /b 0
echo   MISSING dir:    %~1
set /a FAIL+=1
exit /b 0
