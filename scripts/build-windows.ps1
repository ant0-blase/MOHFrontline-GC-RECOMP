param(
    [int]$Jobs = [Environment]::ProcessorCount,
    [ValidateSet("clang","gcc")][string]$Compiler = "clang"
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$MG = Join-Path $Root "ModernGekko"
$RuntimeBuild = Join-Path $Root "build\runtime-windows"
$DolBuild = Join-Path $Root "build\dolrecomp-windows"
$Extracted = Join-Path $Root "extracted"
$Work = Join-Path $Root "port-build\GMFE69\windows-x64"
if (!(Test-Path (Join-Path $Extracted "sys\main.dol"))) {
    throw "Extract your own GMFE69 disc into extracted/ before building Windows."
}
cmake -S $MG -B $RuntimeBuild -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DMODERNGEKKO_ENABLE_DOLPHIN_RUNTIME=ON `
  -DMODERNGEKKO_ENABLE_DOLPHIN_TESTS=OFF
cmake --build $RuntimeBuild --target moderngekko-run -j $Jobs
cmake -S (Join-Path $MG "vendor\dolphin\DolRecomp") -B $DolBuild -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build $DolBuild --target dolrecomp -j $Jobs
$Dol = Get-ChildItem -Path $DolBuild -Filter "dolrecomp.exe" -Recurse | Select-Object -First 1
if (!$Dol) { throw "dolrecomp.exe not found under $DolBuild" }
$Output = Join-Path $Root "module\gGMFE69_recomp.dll"
python (Join-Path $Root "tools\build_all_exec_module.py") `
  --extracted $Extracted --dolrecomp $Dol.FullName --project-root $Root `
  --work $Work --output $Output --game-id GMFE69 --jobs $Jobs `
  --backend c --compiler $Compiler --opt-level 3 --module-type SHARED
$Runner = Get-ChildItem -Path $RuntimeBuild -Filter "moderngekko-run.exe" -Recurse | Select-Object -First 1
if (!$Runner) { throw "moderngekko-run.exe not found" }
New-Item -ItemType Directory -Force -Path (Join-Path $Root "runtime") | Out-Null
Copy-Item $Runner.FullName (Join-Path $Root "runtime\moderngekko-run.exe") -Force
Write-Host "Windows x64 build complete. Run scripts\run-windows.ps1"
