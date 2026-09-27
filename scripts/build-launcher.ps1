param([int]$Jobs = [Environment]::ProcessorCount)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
cmake -S (Join-Path $Root "launcher") -B (Join-Path $Root "build\launcher") -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build (Join-Path $Root "build\launcher") --target MOHFrontline-Launcher MOHFrontline-DiscCache -j $Jobs
Write-Host "Launcher:  $Root\launcher\bin\MOHFrontline-Launcher.exe"
Write-Host "DiscCache: $Root\launcher\bin\MOHFrontline-DiscCache.exe"
