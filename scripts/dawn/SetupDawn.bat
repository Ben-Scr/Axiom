@echo off
REM ============================================================================
REM SetupDawn.bat - Fetch and build the Dawn WebGPU implementation as a
REM                monolithic static library at External\dawn\, then
REM                regenerate Index.sln so the engine is build-ready.
REM
REM Run once after cloning the engine. Subsequent engine builds link against
REM the pre-built webgpu_dawn.lib.
REM
REM Prerequisites:
REM   * Git (with HTTPS access to dawn.googlesource.com)
REM   * Visual Studio 2022 (any edition with C++ workload)
REM   * Python 3 (Dawn's CMake invokes Python for code generation)
REM   * CMake -- either at vendor\cmake\bin\cmake.exe (vendored portable;
REM     run scripts\BootstrapCMake.bat to fetch it) or on PATH.
REM ============================================================================

setlocal EnableDelayedExpansion

pushd "%~dp0\.."

set "DAWN_DIR=External\dawn"
set "DAWN_BUILD_DIR=%DAWN_DIR%\build"
set "VENDOR_CMAKE_EXE=vendor\cmake\bin\cmake.exe"

echo ============================================
echo   Dawn WebGPU Setup
echo ============================================
echo Target: %DAWN_DIR%
echo.

REM --- Tool checks ------------------------------------------------------------
where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] git is not on PATH.
    goto :error
)

REM CMake resolution: prefer vendored portable, then PATH.
set "CMAKE_EXE="
if exist "%VENDOR_CMAKE_EXE%" set "CMAKE_EXE=%VENDOR_CMAKE_EXE%"
if not defined CMAKE_EXE (
    where cmake >nul 2>nul
    if not errorlevel 1 set "CMAKE_EXE=cmake"
)
if not defined CMAKE_EXE (
    echo [ERROR] CMake not found at %VENDOR_CMAKE_EXE% nor on PATH.
    echo         Run scripts\BootstrapCMake.bat to vendor a portable copy,
    echo         or install CMake yourself.
    goto :error
)
echo [info] Using cmake: !CMAKE_EXE!

where python >nul 2>nul
if not errorlevel 1 goto :python_ok
where py >nul 2>nul
if not errorlevel 1 goto :python_ok
echo [ERROR] python / py not on PATH. Dawn's codegen needs Python 3.
goto :error
:python_ok

REM --- Clone ------------------------------------------------------------------
if exist "%DAWN_DIR%\CMakeLists.txt" goto :clone_done
echo [1/5] Cloning Dawn ^(this may take a few minutes^)...
git clone --depth=1 https://dawn.googlesource.com/dawn "%DAWN_DIR%"
if errorlevel 1 (
    echo [ERROR] git clone failed.
    goto :error
)
goto :fetch_deps
:clone_done
echo [1/5] Dawn already present at %DAWN_DIR% ^(skipping clone^).

REM --- Fetch trimmed deps -----------------------------------------------------
REM Dawn's stock fetcher (invoked by -DDAWN_FETCH_DEPENDENCIES=ON) pulls 18
REM submodules with a hardcoded list. We pre-fetch only the 16 we need via
REM our wrapper, then leave DAWN_FETCH_DEPENDENCIES off so CMake doesn't
REM re-pull glfw3 / googletest / google_benchmark.
:fetch_deps
echo [2/5] Fetching Dawn third-party deps ^(trimmed list^)...
where python >nul 2>nul
if not errorlevel 1 (
    python "scripts\FetchDawnDepsMinimal.py"
) else (
    py "scripts\FetchDawnDepsMinimal.py"
)
if errorlevel 1 (
    echo [ERROR] Dep fetch failed.
    goto :error
)

REM --- Configure --------------------------------------------------------------
:configure
echo [3/5] Configuring Dawn via CMake...
"!CMAKE_EXE!" -S "%DAWN_DIR%" -B "%DAWN_BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DDAWN_ENABLE_INSTALL=OFF -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_USE_GLFW=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF
if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    goto :error
)

REM --- Build ------------------------------------------------------------------
echo [4/5] Building webgpu_dawn ^(Debug + Release^)...
"!CMAKE_EXE!" --build "%DAWN_BUILD_DIR%" --config Debug --target webgpu_dawn
if errorlevel 1 (
    echo [ERROR] Debug build failed.
    goto :error
)
"!CMAKE_EXE!" --build "%DAWN_BUILD_DIR%" --config Release --target webgpu_dawn
if errorlevel 1 (
    echo [ERROR] Release build failed.
    goto :error
)

REM --- Regenerate Index.sln ---------------------------------------------------
echo [5/5] Regenerating Index.sln via Premake...
if not exist "vendor\bin\premake5.exe" goto :no_premake
"vendor\bin\premake5.exe" vs2022
if errorlevel 1 echo [WARN] Premake regen returned non-zero. Run manually: vendor\bin\premake5.exe vs2022
goto :done
:no_premake
echo [WARN] vendor\bin\premake5.exe not found. Run scripts\Setup.bat first.

:done
echo.
echo ============================================
echo   Dawn build complete.
echo ============================================
echo Headers : %DAWN_DIR%\include
echo Headers : %DAWN_BUILD_DIR%\gen\include
echo Lib(D)  : %DAWN_BUILD_DIR%\src\dawn\native\Debug\webgpu_dawn.lib
echo Lib(R)  : %DAWN_BUILD_DIR%\src\dawn\native\Release\webgpu_dawn.lib
echo.
echo Open Index.sln in Visual Studio 2022 and build.
echo.

popd
exit /b 0

:error
popd
echo.
echo Setup aborted.
exit /b 1
