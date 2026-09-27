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
if (!(Test-Path $Runtime)) { throw "Missing runtime: $Runtime (run scripts\build-windows.ps1)" }
if (!(Test-Path $Module)) { throw "Missing module: $Module (run scripts\build-windows.ps1)" }
if (!(Test-Path (Join-Path $Game "sys\main.dol"))) { throw "Missing extracted GMFE69 game data" }
New-Item -ItemType Directory -Force -Path $User | Out-Null

if (!$Ps3Files) { $Ps3Files = Join-Path $Root "HD\PS3_FILES" }
New-Item -ItemType Directory -Force -Path $Ps3Files | Out-Null

$env:MOH_PC_SETTINGS_PATH = Join-Path $User "moh_pc_settings.ini"
$env:MOH_PC_INPUT = "1"
$env:MODERNGEKKO_SKIP_ASSET_HASH = "1"
$env:MOH_PS3_FILES = $Ps3Files
$env:MOH_PS3_ASSETS = if ($NoPs3Assets) { "0" } elseif ($Ps3Assets) { "1" } else { if ((Get-ChildItem $Ps3Files -Force -ErrorAction SilentlyContinue | Select-Object -First 1)) { "1" } else { "0" } }
$env:MOH_ENHANCED_GRAPHICS = if ($OriginalGraphics) { "0" } elseif ($EnhancedGraphics) { "1" } else { $env:MOH_PS3_ASSETS }
$env:MOH_PS3_FONTS = "1"
$env:MOH_PS3_FONT_RENDER = "1"
$env:MOH_PS3_FONT_AA = "1"
$env:MOH_PS3_CSM = "0"
$env:MOH_GC_FILES = Join-Path $Game "files"
$env:MOH_NATIVE_PC_ASSETS = "1"
$env:MOH_NATIVE_VFS_DISC = "1"
$env:MOH_NATIVE_AUDIO = "1"

& $Runtime --game $Game --module $Module --user-dir $User --graphics $Graphics
exit $LASTEXITCODE
