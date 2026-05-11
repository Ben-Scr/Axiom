# BootstrapCMake.ps1 - Download + extract the portable Kitware CMake ZIP
# into vendor\cmake\. Called by scripts\BootstrapCMake.bat. Kept as a
# separate PowerShell file because embedding multi-line PowerShell inside
# a .bat (^ line-continuations + single-quoted strings + parens) confused
# cmd.exe's block parser, which silently corrupted SetupDawn.bat's control
# flow. The .bat wrapper is a thin shim; this script does the work.

param(
    [Parameter(Mandatory = $true)] [string] $CMakeVersion,
    [Parameter(Mandatory = $true)] [string] $CMakeUrl,
    [Parameter(Mandatory = $true)] [string] $VendorDir
)

$ErrorActionPreference = 'Stop'

$zipPath = Join-Path $VendorDir 'cmake.zip'
$innerDir = Join-Path $VendorDir ("cmake-{0}-windows-x86_64" -f $CMakeVersion)

Write-Host "  Downloading $CMakeUrl..."
Invoke-WebRequest -UseBasicParsing -Uri $CMakeUrl -OutFile $zipPath

Write-Host "  Extracting..."
Expand-Archive -Force -Path $zipPath -DestinationPath $VendorDir

# The Kitware ZIP unpacks to a nested cmake-X.Y.Z-windows-x86_64\ folder.
# Move its contents up one level so the final layout is
# vendor\cmake\bin\cmake.exe rather than the deeper nested path.
Get-ChildItem -Force $innerDir | Move-Item -Destination $VendorDir
Remove-Item $innerDir -Recurse -Force
Remove-Item $zipPath -Force

Write-Host "  Done."
