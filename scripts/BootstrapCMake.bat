@echo off
REM ============================================================================
REM BootstrapCMake.bat - Download a portable CMake into vendor\cmake\.
REM
REM Useful on machines where group policy blocks winget / system installers
REM (exit code 1625 on managed corporate setups). The portable ZIP from
REM Kitware works without admin: just download + extract.
REM
REM Output: vendor\cmake\bin\cmake.exe and supporting files.
REM Once present, SetupDawn.bat auto-detects and uses this copy.
REM ============================================================================

setlocal

pushd "%~dp0\.."

set "VENDOR_CMAKE_DIR=vendor\cmake"
set "CMAKE_VER=3.30.5"
set "CMAKE_URL=https://github.com/Kitware/CMake/releases/download/v%CMAKE_VER%/cmake-%CMAKE_VER%-windows-x86_64.zip"

if exist "%VENDOR_CMAKE_DIR%\bin\cmake.exe" (
    echo Portable CMake already present at %VENDOR_CMAKE_DIR%\bin\cmake.exe
    "%VENDOR_CMAKE_DIR%\bin\cmake.exe" --version
    popd
    exit /b 0
)

if not exist "%VENDOR_CMAKE_DIR%" mkdir "%VENDOR_CMAKE_DIR%"

echo Downloading CMake %CMAKE_VER% (~50 MB) from Kitware...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0BootstrapCMake.ps1" -CMakeVersion "%CMAKE_VER%" -CMakeUrl "%CMAKE_URL%" -VendorDir "%VENDOR_CMAKE_DIR%"
if errorlevel 1 (
    echo [ERROR] Download / extract failed.
    popd
    exit /b 1
)

echo.
echo CMake ready at %VENDOR_CMAKE_DIR%\bin\cmake.exe
"%VENDOR_CMAKE_DIR%\bin\cmake.exe" --version
popd
exit /b 0
