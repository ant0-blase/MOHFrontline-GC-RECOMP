param(
    [int]$Jobs = [Environment]::ProcessorCount,
    [ValidateSet("clang","gcc")][string]$Compiler = "clang",
    [string]$Iso = ""
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$MG = Join-Path $Root "ModernGekko"
$RuntimeBuild = Join-Path $Root "build\runtime-windows"
$DolBuild = Join-Path $Root "build\dolrecomp-windows"
$Extracted = Join-Path $Root "extracted"
$Work = Join-Path $Root "port-build\GMFE69\windows-x64"

cmake -S (Join-Path $MG "vendor\dolphin\DolRecomp") -B $DolBuild -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build $DolBuild --target dolrecomp -j $Jobs
$Dol = Get-ChildItem -Path $DolBuild -Filter "dolrecomp.exe" -Recurse | Select-Object -First 1
if (!$Dol) { throw "dolrecomp.exe not found under $DolBuild" }

if (!(Test-Path (Join-Path $Extracted "sys\main.dol"))) {
    if (!$Iso) {
        throw "Select your own GMFE69 ISO with -Iso <path> or extract it into extracted/."
    }
    if (!(Test-Path $Iso)) { throw "ISO not found: $Iso" }

    $stream = [System.IO.File]::OpenRead($Iso)
    try {
        $bytes = New-Object byte[] 6
        if ($stream.Read($bytes, 0, 6) -ne 6) { throw "Unable to read GameCube disc ID" }
        $discId = [Text.Encoding]::ASCII.GetString($bytes)
    } finally {
        $stream.Dispose()
    }
    if ($discId -ne "GMFE69") { throw "Unsupported disc ID: $discId (expected GMFE69)" }

    if (Test-Path $Extracted) { Remove-Item -Recurse -Force $Extracted }
    & $Dol.FullName extract $Iso $Extracted
    if ($LASTEXITCODE -ne 0) { throw "DolRecomp extraction failed" }
}

cmake -S $MG -B $RuntimeBuild -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DMODERNGEKKO_ENABLE_DOLPHIN_RUNTIME=ON `
  -DMODERNGEKKO_ENABLE_DOLPHIN_TESTS=OFF
cmake --build $RuntimeBuild --target moderngekko-run -j $Jobs
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
