# Build script for Windows (PowerShell).
# Uses Ninja + clang++ when Ninja is available; falls back to NMake + clang++
# (or NMake + cl.exe if LLVM is not installed).
# Discovers any VS installation via vswhere, including Preview/Insiders builds.
param(
    [string]$BuildDir   = "build",
    [switch]$ForceMSVC,     # skip clang++ and use cl.exe
    [switch]$Verbose        # print what was found/not found
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

function Find-Exe {
    param([string[]]$Names, [string[]]$ExtraDirs = @())
    foreach ($name in $Names) {
        $r = Get-Command $name -ErrorAction SilentlyContinue
        if ($r) { return $r.Source }
    }
    foreach ($dir in $ExtraDirs) {
        foreach ($name in $Names) {
            $p = Join-Path $dir "$name.exe"
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

# clang++: check PATH first, then common LLVM install locations
$clangExe = Find-Exe "clang++" @(
    "C:\Program Files\LLVM\bin",
    "${env:ProgramFiles}\LLVM\bin",
    "C:\LLVM\bin"
)

# ninja: check PATH and CMake's bundled copy
$cmakeBin = $null
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) { $cmakeBin = Split-Path $cmakeCmd.Source }
$ninjaExe = Find-Exe "ninja" (@("C:\Program Files\CMake\bin") + @($cmakeBin | Where-Object { $_ }))

# vswhere: discovers any VS version; -prerelease includes Preview/Insiders
$vswhere = Find-Exe "vswhere" @(
    "C:\Program Files (x86)\Microsoft Visual Studio\Installer",
    "C:\Program Files\Microsoft Visual Studio\Installer"
)
$vsPath = $null
if ($vswhere) {
    # -prerelease is required to find VS Preview / Community Insiders builds
    $vsPath = (& $vswhere -latest -prerelease -property installationPath 2>$null) |
              Where-Object { $_ } | Select-Object -First 1
}

if ($Verbose) {
    Write-Host "clang++ : $(if ($clangExe) { $clangExe } else { 'not found' })"
    Write-Host "ninja   : $(if ($ninjaExe) { $ninjaExe } else { 'not found' })"
    Write-Host "vswhere : $(if ($vswhere)  { $vswhere  } else { 'not found' })"
    Write-Host "VS path : $(if ($vsPath)   { $vsPath   } else { 'not found' })"
}

# ── Validate we have at least one usable toolchain ────────────────────────────

if (-not $vsPath -and -not $clangExe) {
    throw ("Neither a Visual Studio installation nor LLVM clang++ was found.`n" +
           "Install one of:`n" +
           "  Visual Studio Build Tools (any version, including Preview)`n" +
           "  LLVM: https://github.com/llvm/llvm-project/releases")
}

# ── Import VS build environment ───────────────────────────────────────────────
# Always do this when VS is available — clang++ on Windows still needs the
# MSVC linker (link.exe) and Windows SDK headers for DLL creation.

if ($vsPath) {
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path $vcvars) {
        Write-Host "Importing VS build environment (x64) from $vsPath ..."
        $dump = cmd /c "`"$vcvars`" x64 2>&1 && set"
        foreach ($line in $dump) {
            if ($line -match '^([^=]+)=(.*)$') {
                [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            }
        }
    } else {
        Write-Warning "vcvarsall.bat not found in $vsPath; proceeding without VS environment"
    }
}

# ── Choose generator and set compiler via environment ─────────────────────────
# We set $env:CXX instead of passing -DCMAKE_CXX_COMPILER because paths that
# contain spaces (e.g. "C:\Program Files\LLVM\...") are mangled when embedded
# inside a -D flag string and then PowerShell-splatted as an array element.
# CMake reads CXX/CC env vars before any -D flags, so this is the clean path.

$useClang = $clangExe -and -not $ForceMSVC

if ($useClang -and $ninjaExe) {
    $generator = "Ninja"
    $env:CXX   = $clangExe
    $makeFlag  = "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
    Write-Host "Toolchain: Ninja + clang++ ($clangExe)"
} elseif ($useClang) {
    $generator = "NMake Makefiles"
    $env:CXX   = $clangExe
    $makeFlag  = $null
    Write-Host "Toolchain: NMake + clang++ ($clangExe)"
} else {
    $generator = "NMake Makefiles"
    Remove-Item Env:CXX -ErrorAction SilentlyContinue   # let cmake find cl.exe
    $makeFlag  = $null
    Write-Host "Toolchain: NMake + MSVC cl.exe"
}

# ── Stale cache check ─────────────────────────────────────────────────────────
# Wipe the build dir when the cached generator no longer matches.

$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cachedGen = (Select-String "CMAKE_GENERATOR:INTERNAL=" $cacheFile |
                  Select-Object -First 1)?.Line -replace "^.*=",""
    if ($cachedGen -and $cachedGen.Trim() -ne $generator) {
        Write-Host "Removing stale build dir (cached generator '$($cachedGen.Trim())' != '$generator')"
        Remove-Item -Recurse -Force $BuildDir
    }
}

# ── CMake configure + build ───────────────────────────────────────────────────

$cmakeArgs = @("-B", $BuildDir, "-G", $generator, "-DCMAKE_BUILD_TYPE=Release")
if ($makeFlag) { $cmakeArgs += $makeFlag }

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }

& cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

# ── Report ─────────────────────────────────────────────────────────────────────

$dll = Get-ChildItem -Path $BuildDir -Filter "qrunch_qsim.dll" -Recurse -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName

if (-not $dll) { throw "Build completed but qrunch_qsim.dll was not found under $BuildDir" }

Write-Host ""
Write-Host "Built: $dll"
Write-Host ""
Write-Host "To activate in Julia (run once per session, or add to your profile):"
Write-Host "  `$env:QRUNCH_QSIM_LIB = `"$dll`""
