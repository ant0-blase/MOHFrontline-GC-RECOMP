param(
    [Parameter(Mandatory=$true)][string]$DiscImage,
    [int]$Jobs = [Environment]::ProcessorCount
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Toolchain = Join-Path $Root "toolchain"
$Cache = Join-Path $Root "disc-cache\GMFE69"
$Work = Join-Path $Root "port-build\GMFE69\portable-windows"
$Python = Join-Path $Toolchain "python\python.exe"
$DolRecomp = Join-Path $Toolchain "bin\dolrecomp.exe"
$DiscCache = Join-Path $Root "MOHFrontline-DiscCache.exe"
if (!(Test-Path $DiscCache)) { $DiscCache = Join-Path $Root "launcher\bin\MOHFrontline-DiscCache.exe" }

if (!(Test-Path $DiscImage)) { throw "Selected GMFE69 disc image does not exist: $DiscImage" }
$RequiredFiles = @(
    (Join-Path $Toolchain "cmake\bin\cmake.exe"),
    (Join-Path $Toolchain "bin\ninja.exe"),
    (Join-Path $Toolchain "zig\zig.exe"),
    $Python,
    $DolRecomp,
    $DiscCache,
    (Join-Path $Root "runtime\moderngekko-run.exe")
)
foreach ($Required in $RequiredFiles) {
    if (!(Test-Path $Required)) { throw "Portable package is incomplete: $Required" }
}

New-Item -ItemType Directory -Force (Join-Path $Root "user"), (Join-Path $Root "module"), (Join-Path $Root "port-build") | Out-Null
$Log = Join-Path $Root "user\launcher-build.log"
Start-Transcript -Path $Log -Force | Out-Null
try {
    $env:PATH = "$(Join-Path $Toolchain 'cmake\bin');$(Join-Path $Toolchain 'bin');$(Join-Path $Toolchain 'zig');$env:PATH"
    $env:CMAKE_GENERATOR = "Ninja"

    Write-Host "==> Building minimal executable cache with encounter/nod"
    & $DiscCache $DiscImage $Cache
    if ($LASTEXITCODE -ne 0) { throw "Disc cache failed with exit code $LASTEXITCODE" }

    (Resolve-Path $DiscImage).Path | Set-Content (Join-Path $Root "user\disc-image.txt") -NoNewline

    Write-Host "==> Recompiling GMFE69 with bundled portable toolchain"
    $ArgsList = @(
      (Join-Path $Root "tools\build_all_exec_module.py"),
      "--extracted", $Cache,
      "--dolrecomp", $DolRecomp,
      "--project-root", $Root,
      "--work", $Work,
      "--output", (Join-Path $Root "module\gGMFE69_recomp.dll"),
      "--game-id", "GMFE69",
      "--jobs", "$Jobs",
      "--backend", "c",
      "--opt-level", "3",
      "--module-type", "SHARED",
      "--cmake-toolchain", (Join-Path $Toolchain "moh-zig.cmake")
    )
    & $Python @ArgsList
    if ($LASTEXITCODE -ne 0) { throw "Native module build failed with exit code $LASTEXITCODE" }

    Write-Host "==> Ready: runtime will mount the original disc image directly"
} finally {
    Stop-Transcript | Out-Null
}
