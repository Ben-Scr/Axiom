@echo off
REM Materialize a self-contained EngineSDK folder under a consumer's
REM target directory. Invoked from the Index-Launcher postbuild via the
REM CopyEngineSdk variable in premake5.lua.
REM
REM Args:
REM   %1 = SDK target dir, e.g. ...\bin\Debug-windows-x86_64\Index-Launcher\EngineSDK
REM   %2 = engine repo root, the ROOT_DIR value from premake
REM   %3 = config-specific bin dir, e.g. ...\bin\Debug-windows-x86_64
REM
REM robocopy exit codes 0..7 are success; 8+ are failures. Each step uses
REM `if errorlevel 8 goto :robofail` rather than a multi-line if-block,
REM because cmd.exe paren-balances aggressively across compound statements
REM and any literal parens in echo arguments inside an if-block confuse it.
REM /NJH /NJS /NDL /NP /NS /NC suppress robocopy's banner/per-file noise.

setlocal EnableDelayedExpansion

set "SDK=%~1"
set "ROOT=%~2"
set "BIN=%~3"

if "%SDK%"=="" goto :missingSdk
if "%ROOT%"=="" goto :missingRoot
if "%BIN%"=="" goto :missingBin

set "ROBOFLAGS=/NJH /NJS /NDL /NP /NS /NC"

REM --- Index-Engine headers and sources. CMakeLists.txt adds this as an
REM     include path; .cpp files are harmless dead weight we accept here.
robocopy "%ROOT%\Index-Engine\src" "%SDK%\Index-Engine\src" /MIR %ROBOFLAGS% >nul
if errorlevel 8 goto :robofail

REM --- Index-ScriptCore csproj and dll. The csproj is what user projects
REM     reference for compilation; the dll is the runtime artifact that
REM     ScriptSystem loads. Mirror only the two files we need.
robocopy "%ROOT%\Index-ScriptCore" "%SDK%\Index-ScriptCore" Index-ScriptCore.csproj %ROBOFLAGS% >nul
if errorlevel 8 goto :robofail
robocopy "%BIN%\Index-ScriptCore" "%SDK%\Index-ScriptCore" Index-ScriptCore.dll %ROBOFLAGS% >nul
if errorlevel 8 goto :robofail

REM --- External vendored deps. Only the directories CMakeLists references
REM     get shipped. Each entry mirrors a leaf directory so we do not pull
REM     in build outputs, .git folders, or test trees from the submodules.
call :CopyExternalDir spdlog\include
if errorlevel 1 goto :robofail
call :CopyExternalDir glm
if errorlevel 1 goto :robofail
call :CopyExternalDir entt\src
if errorlevel 1 goto :robofail
call :CopyExternalDir stb
if errorlevel 1 goto :robofail
call :CopyExternalDir magic_enum\include
if errorlevel 1 goto :robofail
call :CopyExternalDir cereal\include
if errorlevel 1 goto :robofail
call :CopyExternalDir glfw\include
if errorlevel 1 goto :robofail
call :CopyExternalDir glad\include
if errorlevel 1 goto :robofail
call :CopyExternalDir miniaudio
if errorlevel 1 goto :robofail
call :CopyExternalDir box2d\include
if errorlevel 1 goto :robofail
call :CopyExternalDir Axiom-Physics\include
if errorlevel 1 goto :robofail
call :CopyExternalDir dotnet
if errorlevel 1 goto :robofail

REM --- Index-Engine.lib for the active config so user NativeScripts link.
REM     The lib lives in <bin>\<cfg>-<plat>\Index-Engine\ and the generated
REM     CMakeLists.txt looks for it under ${INDEX_DIR}\bin\<cfg>-<plat>\
REM     Index-Engine\, so we mirror the same relative layout inside the SDK.
for %%I in ("%BIN%") do set "CFGPLAT=%%~nxI"
robocopy "%BIN%\Index-Engine" "%SDK%\bin\%CFGPLAT%\Index-Engine" Index-Engine.lib %ROBOFLAGS% >nul
if errorlevel 8 goto :robofail

endlocal
exit /b 0

:CopyExternalDir
REM %1 = sub-path inside External\ to mirror into the SDK. Missing submodules
REM are skipped silently so a partial checkout still builds; the
REM verify-standalone-zip script catches gaps that matter.
set "REL=%~1"
if not exist "%ROOT%\External\%REL%" exit /b 0
robocopy "%ROOT%\External\%REL%" "%SDK%\External\%REL%" /MIR %ROBOFLAGS% >nul
if errorlevel 8 goto :externalFail
exit /b 0

:externalFail
echo CopyEngineSdk: robocopy failed mirroring External\%REL% with errorlevel %errorlevel% 1>&2
exit /b 1

:robofail
echo CopyEngineSdk: robocopy failed with errorlevel %errorlevel% 1>&2
endlocal
exit /b 1

:missingSdk
echo CopyEngineSdk: missing target SDK dir argument 1>&2
endlocal
exit /b 1

:missingRoot
echo CopyEngineSdk: missing repo root argument 1>&2
endlocal
exit /b 1

:missingBin
echo CopyEngineSdk: missing bin dir argument 1>&2
endlocal
exit /b 1
