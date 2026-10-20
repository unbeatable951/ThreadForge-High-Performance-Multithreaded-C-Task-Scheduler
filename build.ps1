#!/usr/bin/env pwsh
# =============================================================================
# ThreadForge Build Script
# Usage: .\build.ps1 [configure|build|test|bench|examples|clean|all]
# =============================================================================

param(
    [string]$Action = "all",
    [string]$BuildType = "Release",
    [switch]$ASAN,
    [switch]$TSAN
)

$ErrorActionPreference = "Stop"

# ---- Toolchain Setup --------------------------------------------------------
$MINGW_PATH = "d:\tools\mingw64\bin"
if (Test-Path $MINGW_PATH) {
    $env:PATH = "$MINGW_PATH;$env:PATH"
    Write-Host "[*] Using MinGW-w64 GCC from: $MINGW_PATH" -ForegroundColor Cyan
} else {
    Write-Host "[*] Using system GCC/CMake" -ForegroundColor Yellow
}

$BUILD_DIR = "build"

function Invoke-Configure {
    Write-Host "`n[*] Configuring CMake ($BuildType)..." -ForegroundColor Cyan
    $args_list = @(
        "-S", ".",
        "-B", $BUILD_DIR,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_CXX_COMPILER=g++"
    )
    if ($ASAN)  { $args_list += "-DENABLE_ASAN=ON" }
    if ($TSAN)  { $args_list += "-DENABLE_TSAN=ON" }

    & cmake @args_list
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    Write-Host "[+] Configuration done." -ForegroundColor Green
}

function Invoke-Build {
    Write-Host "`n[*] Building ThreadForge..." -ForegroundColor Cyan
    & cmake --build $BUILD_DIR --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
    Write-Host "[+] Build successful." -ForegroundColor Green
}

function Invoke-Tests {
    Write-Host "`n[*] Running Test Suite..." -ForegroundColor Cyan
    & .\$BUILD_DIR\threadforge_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
    Write-Host "[+] All tests passed." -ForegroundColor Green
}

function Invoke-Bench {
    Write-Host "`n[*] Running Performance Benchmarks..." -ForegroundColor Cyan
    & .\$BUILD_DIR\threadforge_bench.exe
    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed" }
}

function Invoke-Examples {
    Write-Host "`n[*] Running Examples..." -ForegroundColor Cyan
    Write-Host "--- Basic Usage ---" -ForegroundColor Yellow
    & .\$BUILD_DIR\example_basic_usage.exe
    Write-Host "--- DAG Pipeline ---" -ForegroundColor Yellow
    & .\$BUILD_DIR\example_dag_pipeline.exe
    Write-Host "--- PTX Compiler Simulation ---" -ForegroundColor Yellow
    & .\$BUILD_DIR\example_ptx_simulation.exe
}

function Invoke-Clean {
    Write-Host "`n[*] Cleaning build directory..." -ForegroundColor Cyan
    if (Test-Path $BUILD_DIR) {
        Remove-Item -Recurse -Force $BUILD_DIR
        Write-Host "[+] Cleaned." -ForegroundColor Green
    } else {
        Write-Host "[~] Nothing to clean." -ForegroundColor Yellow
    }
}

switch ($Action.ToLower()) {
    "configure" { Invoke-Configure }
    "build"     { Invoke-Configure; Invoke-Build }
    "test"      { Invoke-Tests }
    "bench"     { Invoke-Bench }
    "examples"  { Invoke-Examples }
    "clean"     { Invoke-Clean }
    "all" {
        Invoke-Configure
        Invoke-Build
        Invoke-Tests
        Invoke-Bench
        Invoke-Examples
    }
    default {
        Write-Host "Unknown action: $Action" -ForegroundColor Red
        Write-Host "Usage: .\build.ps1 [configure|build|test|bench|examples|clean|all]"
        exit 1
    }
}
