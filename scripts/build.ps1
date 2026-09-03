<#
.SYNOPSIS
    Builds the Babylon.js USD importer WebAssembly module, TypeScript package, and demo.

.DESCRIPTION
    Configures OpenUSD through vcpkg, builds the standalone command-buffer Wasm module,
    builds the npm package, then emits the browser demo under docs/. The repository has no
    git submodules.

.PARAMETER Clean
    Remove generated build and package output before configuring.
#>
[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$ProjectDir = Split-Path -Parent $PSScriptRoot

if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT is not set. Point it at your vcpkg checkout."
}
if (-not (Get-Command emcc -ErrorAction SilentlyContinue) -and
    -not $env:EMSDK -and -not $env:EMSCRIPTEN_ROOT) {
    throw "Emscripten not found. Put emcc on PATH, or set EMSDK / EMSCRIPTEN_ROOT."
}
if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {
    throw "npm was not found on PATH."
}

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $ninja = Get-ChildItem "$env:VCPKG_ROOT/downloads/tools" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $ninja) {
        throw "Ninja was not found on PATH or in the vcpkg downloads directory."
    }
    $env:PATH = "$($ninja.DirectoryName);$env:PATH"
}

Push-Location $ProjectDir
try {
    if ($Clean) {
        Remove-Item -Recurse -Force "build" -ErrorAction SilentlyContinue
        Remove-Item -Recurse -Force "package/dist" -ErrorAction SilentlyContinue
    }

    Write-Host "`n[1/3] Building OpenUSD Wasm and package" -ForegroundColor Cyan
    cmake --workflow --preset wasm-package
    if ($LASTEXITCODE -ne 0) { throw "Wasm/package build failed." }

    Write-Host "`n[2/3] Installing demo dependencies" -ForegroundColor Cyan
    npm --prefix demo install --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw "Demo dependency install failed." }

    Write-Host "`n[3/3] Building demo" -ForegroundColor Cyan
    npm --prefix demo run build
    if ($LASTEXITCODE -ne 0) { throw "Demo build failed." }

    Write-Host "`nBuild complete." -ForegroundColor Green
    Get-ChildItem "build/wasm-package/bin" |
        Select-Object Name, @{ Name = 'MB'; Expression = { [math]::Round($_.Length / 1MB, 2) } } |
        Format-Table -AutoSize
}
finally {
    Pop-Location
}
