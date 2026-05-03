# Build script for Windows (PowerShell).
# Preferred toolchain: Ninja + clang++ (LLVM), using VS for the MSVC linker/SDK.
# Fallback toolchain: NMake + MSVC cl.exe.
# No Visual Studio year is hardcoded; vswhere discovers whatever is installed.
param(
    [string]$BuildDir = "build",
    [switch]$ForceClang,    # always use clang++ even if MSVC fallback would work
    [switch]$ForceMSVC      # always use cl.exe even if clang++ is available
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
Set-Location $Root

# ── Fetch dependencies ─────────────────────────────────────────────────────────

if (-not (Test-Path "extern/qsim/lib/circuit.h")) {
    Write-Host "Fetching qsim submodule..."
    git submodule update --init extern/qsim
}

if (-not (Test-Path "extern/nlohmann/json.hpp")) {
    Write-Host "Downloading nlohmann/json.hpp..."
    New-Item -ItemType Directory -Force extern/nlohmann | Out-Null
    Invoke-WebRequest -Uri "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" `
                      -OutFile "extern/nlohmann/json.hpp"
}

# ── Discover tools ─────────────────────────────────────────────────────────────

# vswhere finds any VS installation, year-independent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = $null
if (Test-Path $vswhere) {
    $vsPath = (& $vswhere -latest -property installationPath 2>$null) | Select-Object -First 1
    if ($vsPath) { Write-Host "Found VS at: $vsPath" }
}

# Inject VS build environment (linker, Windows SDK, CRT headers) into this session.
# This is safe to call even when using clang++ as the compiler — the MSVC linker
# and Windows SDK are still needed for the final .dll link step.
function Import-VSEnvironment {
    param([string]$VsPath)
    $vcvars = Join-Path $VsPath "VC\Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path $vcvars)) {
        Write-Warning "vcvarsall.bat not found at $vcvars; skipping VS environment import"
        return
    }
    Write-Host "Importing VS build environment (x64)..."
    $dump = cmd /c "`"$vcvars`" x64 2>&1 && set"
    foreach ($line in $dump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

# Find ninja: check PATH first, then CMake's own bin directory
$ninjaCmd = $null
$ninjaCandidate = Get-Command ninja -ErrorAction SilentlyContinue
if ($ninjaCandidate) {
    $ninjaCmd = $ninjaCandidate.Source
} else {
    $cmakeDir = Split-Path (Get-Command cmake -ErrorAction Stop).Source
    $bundled  = Join-Path $cmakeDir "ninja.exe"
    if (Test-Path $bundled) { $ninjaCmd = $bundled }
}

$clangCmd = (Get-Command clang++ -ErrorAction SilentlyContinue)?.Source

# ── Choose toolchain ───────────────────────────────────────────────────────────

$useClang = ($clangCmd -and $ninjaCmd -and -not $ForceMSVC) -or $ForceClang

if ($useClang -and -not $clangCmd) {
    throw "clang++ not found in PATH. Install LLVM or remove -ForceClang."
}
if ($useClang -and -not $ninjaCmd) {
    throw "ninja not found. Install via: winget install Ninja-build.Ninja"
}

# ── Build ──────────────────────────────────────────────────────────────────────

if ($vsPath) { Import-VSEnvironment $vsPath }

if ($useClang) {
    Write-Host "Toolchain: Ninja + clang++ ($clangCmd)"
    cmake -B $BuildDir `
          -G Ninja `
          "-DCMAKE_MAKE_PROGRAM=$ninjaCmd" `
          "-DCMAKE_CXX_COMPILER=$clangCmd" `
          -DCMAKE_BUILD_TYPE=Release
    cmake --build $BuildDir --parallel
} else {
    Write-Host "Toolchain: NMake + MSVC cl.exe"
    if (-not $vsPath) {
        throw ("No VS installation found and no clang++ available. " +
               "Install Visual Studio Build Tools or LLVM.")
    }
    cmake -B $BuildDir `
          -G "NMake Makefiles" `
          -DCMAKE_BUILD_TYPE=Release
    cmake --build $BuildDir --parallel
}

# ── Report output path ─────────────────────────────────────────────────────────

$dll = Get-ChildItem -Path $BuildDir -Filter "qrunch_qsim.dll" -Recurse -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName

if (-not $dll) {
    throw "Build completed but qrunch_qsim.dll was not found under $BuildDir"
}

Write-Host ""
Write-Host "Built: $dll"
Write-Host ""
Write-Host "To use in Julia, run once per session (or add to your profile):"
Write-Host "  `$env:QRUNCH_QSIM_LIB = `"$dll`""
