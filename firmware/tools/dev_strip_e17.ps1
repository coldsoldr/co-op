# e17 channel strip device test (docs/device-tests.md "e17"): stats before, the scenario tests/sim/dev_strip_e17.txt
# through devloop.py (drives the OP-1 over USB MIDI, records the UR44C inputs 5-6), stats after.
# Run from firmware/:  powershell -ExecutionPolicy Bypass -File tools\dev_strip_e17.ps1 [-Scenario FILE] [-Out DIR]
param([string]$Scenario = "tests\sim\dev_strip_e17.txt",
      [string]$Out = "I:\op1emu-tmp\devloop-e17\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
$ErrorActionPreference = "Stop"
$py = "I:\op1emu-tmp\devloop-venv\Scripts\python.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
Write-Host "results: $Out"

& $py tools\coop_usb.py hello
if ($LASTEXITCODE -ne 0) { throw "no OP-1 on USB (close the co-op browser tab if the port is busy)" }
& $py tools\coop_usb.py stats -o "$Out\stats_before.txt"
if ($LASTEXITCODE -ne 0) { throw "stats before failed" }

& $py tools\devloop.py $Scenario --out "$Out\run" --allow-tape --wait-port 30
if ($LASTEXITCODE -ne 0) { throw "devloop failed" }

Start-Sleep -Seconds 3
& $py tools\coop_usb.py stats -o "$Out\stats_after.txt"
Write-Host "--- strip line before / after ---"
Select-String -Path "$Out\stats_before.txt", "$Out\stats_after.txt" -Pattern "^(strip|sui)" | ForEach-Object { $_.Line }
Write-Host "done: $Out"
