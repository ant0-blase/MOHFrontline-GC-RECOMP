param(
    [string]$Graphics = "Vulkan",
    [string]$Ps3Files = "",
    [switch]$Ps3Assets,
    [switch]$NoPs3Assets,
    [switch]$EnhancedGraphics,
    [switch]$OriginalGraphics
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Runtime = Join-Path $Root "runtime\moderngekko-run.exe"
$Module = Join-Path $Root "module\gGMFE69_recomp.dll"
$Game = Join-Path $Root "extracted"
$User = Join-Path $Root "user"
$DiscSourceFile = Join-Path $User "disc-image.txt"
if (Test-Path $DiscSourceFile) {
    $DiscSource = (Get-Content $DiscSourceFile -Raw).Trim()
    if ($DiscSource -and (Test-Path $DiscSource)) { $Game = $DiscSource }
}
if (!(Test-Path $Runtime)) { throw "Missing runtime: $Runtime (run scripts\build-windows.ps1)" }
if (!(Test-Path $Module)) { throw "Missing module: $Module (run scripts\build-windows.ps1)" }
if (!(Test-Path $Game)) { throw "Missing selected GMFE69 game source: $Game" }
if ((Get-Item $Game).PSIsContainer -and !(Test-Path (Join-Path $Game "sys\main.dol"))) {
    throw "Incomplete extracted GMFE69 game data"
}
New-Item -ItemType Directory -Force -Path $User | Out-Null

if (!$Ps3Files) {
    $Ps3Files = Join-Path $Root "HD\PS3_FILES"
    New-Item -ItemType Directory -Force -Path $Ps3Files | Out-Null
}

$Ps3SourceExists = Test-Path $Ps3Files
$Ps3SourceItem = if ($Ps3SourceExists) { Get-Item $Ps3Files } else { $null }
$Ps3IsPkg = $Ps3SourceItem -and !$Ps3SourceItem.PSIsContainer -and
    $Ps3SourceItem.Extension.Equals(".pkg", [StringComparison]::OrdinalIgnoreCase)
$Ps3DirectoryHasFiles = $Ps3SourceItem -and $Ps3SourceItem.PSIsContainer -and
    [bool](Get-ChildItem $Ps3Files -File -Recurse -Force -ErrorAction SilentlyContinue | Select-Object -First 1)

$env:MOH_PC_SETTINGS_PATH = Join-Path $User "moh_pc_settings.ini"
$env:MOH_PC_INPUT = "1"
$env:MODERNGEKKO_SKIP_ASSET_HASH = "1"
$env:MOH_PS3_ASSETS = if ($NoPs3Assets) { "0" } elseif ($Ps3Assets) { "1" } else {
    if ($Ps3IsPkg -or $Ps3DirectoryHasFiles) { "1" } else { "0" }
}

Remove-Item Env:MOH_PS3_FILES -ErrorAction SilentlyContinue
Remove-Item Env:MOH_PS3_PKG -ErrorAction SilentlyContinue
if ($env:MOH_PS3_ASSETS -eq "1") {
    if ($Ps3IsPkg) {
        $env:MOH_PS3_PKG = (Resolve-Path $Ps3Files).Path
        Write-Host "PS3 remaster PKG (extraction-less): $env:MOH_PS3_PKG"
    } elseif ($Ps3SourceItem -and $Ps3SourceItem.PSIsContainer) {
        $env:MOH_PS3_FILES = (Resolve-Path $Ps3Files).Path
    } else {
        throw "PS3 asset source must be a PS3_FILES directory or finalized .pkg: $Ps3Files"
    }
}
$env:MOH_ENHANCED_GRAPHICS = if ($OriginalGraphics) { "0" } elseif ($EnhancedGraphics) { "1" } else { $env:MOH_PS3_ASSETS }
$env:MOH_PS3_FONTS = "1"
$env:MOH_PS3_FONT_RENDER = "1"
$env:MOH_PS3_FONT_AA = "1"
$env:MOH_PS3_CSM = "0"
if ((Get-Item $Game).PSIsContainer -and (Test-Path (Join-Path $Game "files"))) {
    $env:MOH_GC_FILES = Join-Path $Game "files"
} else {
    Remove-Item Env:MOH_GC_FILES -ErrorAction SilentlyContinue
}
if ((Get-Item $Game).PSIsContainer) {
    Remove-Item Env:MOH_NATIVE_DISC_IMAGE -ErrorAction SilentlyContinue
} else {
    $env:MOH_NATIVE_DISC_IMAGE = (Resolve-Path $Game).Path
}
$env:MOH_NATIVE_PC_ASSETS = "1"
$env:MOH_NATIVE_VFS_DISC = "1"
$env:MOH_NATIVE_AUDIO = "1"

& $Runtime --game $Game --module $Module --user-dir $User --graphics $Graphics
exit $LASTEXITCODE
