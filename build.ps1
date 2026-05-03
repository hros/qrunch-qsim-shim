# Quick local build script for Windows (PowerShell).
# Requires: Visual Studio 2022 (or Build Tools) + CMake + git.
param(
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

Set-Location $Root

# Fetch qsim submodule if missing
if (-not (Test-Path "extern/qsim/lib/circuit.h")) {
    Write-Host "Fetching qsim submodule..."
    git submodule update --init extern/qsim
}

# Fetch nlohmann/json if missing
if (-not (Test-Path "extern/nlohmann/json.hpp")) {
    Write-Host "Downloading nlohmann/json.hpp..."
    New-Item -ItemType Directory -Force extern/nlohmann | Out-Null
    Invoke-WebRequest -Uri "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" `
                      -OutFile "extern/nlohmann/json.hpp"
}

cmake -B $BuildDir -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022" -A x64
cmake --build $BuildDir --config Release --parallel

$dll = Join-Path $Root "$BuildDir\Release\qrunch_qsim.dll"
Write-Host ""
Write-Host "Built: $dll"
Write-Host "Set in Julia:"
Write-Host "  `$env:QRUNCH_QSIM_LIB = `"$dll`""
