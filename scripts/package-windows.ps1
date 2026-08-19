param([string]$Output = "")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (!$Output) { $Output = Join-Path $Root "dist\MOHFrontline-PC-windows-x64" }
if (Test-Path $Output) { Remove-Item -Recurse -Force $Output }
New-Item -ItemType Directory -Force -Path $Output, "$Output\runtime", "$Output\module", "$Output\extracted", "$Output\user" | Out-Null
Copy-Item "$Root\runtime\moderngekko-run.exe" "$Output\runtime\"
if (Test-Path "$Root\runtime\Sys") { Copy-Item -Recurse "$Root\runtime\Sys" "$Output\runtime\Sys" }
Copy-Item "$Root\module\gGMFE69_recomp.dll" "$Output\module\"
Copy-Item "$Root\scripts\run-windows.ps1" "$Output\"
Copy-Item "$Root\LICENSE" "$Output\"
"Extract your legally owned GMFE69 game here. Retail data is not distributed." | Set-Content "$Output\extracted\README.txt"
Compress-Archive -Path "$Output\*" -DestinationPath "$Output.zip" -Force
Write-Host "Created $Output.zip"
