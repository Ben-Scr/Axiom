@echo off
REM ============================================================================
REM   Axiom test runner
REM
REM   Builds (incremental) and runs every Tests/<Name> binary in Debug x64.
REM   Run from the repo root; pass --release for Release configuration.
REM   Exits non-zero on first failing target so CI can gate on it.
REM ============================================================================

setlocal EnableDelayedExpansion

set "CONFIG=Debug"
if /I "%~1"=="--release" set "CONFIG=Release"
if /I "%~1"=="--dist" set "CONFIG=Dist"

pushd "%~dp0\.."

REM Prefer MSBUILD_PATH from scripts\axiom-build-env.bat (written by Setup.py
REM via vswhere). Fall back to the historical Community-edition default so
REM running RunTests.bat without first running Setup.py still works.
set "AXIOM_BUILD_ENV=%~dp0axiom-build-env.bat"
if exist "%AXIOM_BUILD_ENV%" call "%AXIOM_BUILD_ENV%"

set "MSBUILD=%MSBUILD_PATH%"
if not defined MSBUILD set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

if not exist "%MSBUILD%" (
    echo [ERROR] MSBuild not found at: %MSBUILD%
    echo         Run "python scripts\Setup.py" to detect MSBuild via vswhere,
    echo         or install Visual Studio 2022 with the C++ workload.
    popd
    exit /b 1
)

if not exist "Axiom.sln" (
    echo [INFO] No solution found, regenerating with premake5...
    call vendor\bin\premake5.exe vs2022 || (
        echo [ERROR] premake5 failed.
        popd
        exit /b 1
    )
)

echo [INFO] Building Axiom-Engine-Tests (%CONFIG% x64)...
"%MSBUILD%" "Tests\Axiom-Engine-Tests\Axiom-Engine-Tests.vcxproj" -p:Configuration=%CONFIG% -p:Platform=x64 -verbosity:minimal -nologo
if errorlevel 1 (
    echo [ERROR] Build failed.
    popd
    exit /b 1
)

set "TEST_EXE=bin\%CONFIG%-windows-x86_64\Axiom-Engine-Tests\Axiom-Engine-Tests.exe"
if not exist "%TEST_EXE%" (
    echo [ERROR] Test binary not produced: %TEST_EXE%
    popd
    exit /b 1
)

echo.
echo [INFO] Running %TEST_EXE%...
echo.
"%TEST_EXE%" %2 %3 %4 %5 %6 %7 %8 %9
set "RESULT=%ERRORLEVEL%"

popd
exit /b %RESULT%
