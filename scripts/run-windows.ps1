param(
    [string]$Graphics = "Vulkan"
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
$env:MOH_PC_SETTINGS_PATH = Join-Path $User "moh_pc_settings.ini"
$env:MOH_PC_INPUT = "1"
& $Runtime --game $Game --module $Module --user-dir $User --graphics $Graphics
exit $LASTEXITCODE
