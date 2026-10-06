# Packs the current emulator and shadnet builds into a co-op kit for playing over the internet:
#   <Out>\shadps4.exe            snapshot of out\emulator\build (both players must run the same one)
#   <Out>\server                 shadnet on 0.0.0.0, ports 31313 (TCP), 31314 (UDP), 31315 (WebAPI)
#   <Out>\backend                the local ss4 backend (port 20443)
#   <Out>\*.bat, tools\setup.ps1 from tools\online-kit
# and zips it (without any player's user dir or settings) as <Out>.zip to send to the other player.
# See docs/ONLINE_TEST.md.
param(
    [string]$Out = "C:\bb\online-kit",
    [string]$Install = "C:\Bloodborne.PC.Online",
    [switch]$Force
)
$ErrorActionPreference = "Stop"
if (-not $env:BB_ROOT) { $env:BB_ROOT = "C:\bb" }
$src = Join-Path $PSScriptRoot "online-kit"

if ((Test-Path $Out) -and -not $Force) { throw "$Out exists; pass -Force to refresh it (the user dir and settings are kept)" }
New-Item -ItemType Directory -Force "$Out\server\clientsample", "$Out\backend", "$Out\tools" | Out-Null

Copy-Item (Join-Path $env:BB_ROOT "out\emulator\build\shadps4.exe") "$Out\" -Force

Copy-Item (Join-Path $env:BB_ROOT "out\server\shadnet.exe") "$Out\server\" -Force
cmd /c "`"C:\Qt\6.8.3\msvc2022_64\bin\windeployqt6.exe`" --release --no-translations `"$Out\server\shadnet.exe`" >nul 2>nul"
if (-not (Test-Path "$Out\server\Qt6Core.dll")) { throw "windeployqt failed for $Out\server\shadnet.exe" }
Copy-Item "$Install\DATA\shadNet-P2P\clientsample\shadnet-sample.exe" "$Out\server\clientsample\" -Force
@"
[General]
Host=0.0.0.0
UnsecuredPort=31313
MatchingUdpPort=31314
WebApiPort=31315
StatsEnabled=false
Matching2Enabled=true
BloodborneSeamlessCoop=true
EmailValidated=false
AdminsList=
RegistrationSecretKey=
"@ | Set-Content "$Out\server\shadnet.cfg" -Encoding ascii

Copy-Item "$PSScriptRoot\..\backend\*.py", "$PSScriptRoot\..\backend\ss_info_template.xml" "$Out\backend\" -Force
if (-not (Test-Path "$Out\backend\notice.txt")) { "" | Set-Content "$Out\backend\notice.txt" }

# cmd needs CRLF for labels and goto; the repo may hand us LF.
foreach ($f in Get-ChildItem $src -Recurse -File) {
    $rel = $f.FullName.Substring($src.Length + 1)
    $text = [IO.File]::ReadAllText($f.FullName) -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText((Join-Path $Out $rel), $text, (New-Object Text.ASCIIEncoding))
}

$zip = "$Out.zip"
if (Test-Path $zip) { Remove-Item $zip }
$items = Get-ChildItem $Out | Where-Object { $_.Name -notin @("user", "settings.bat") }
Compress-Archive -Path $items.FullName -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash "$Out\shadps4.exe" -Algorithm SHA256).Hash.Substring(0, 12)
Write-Host "Kit ready at $Out"
Write-Host "Send $zip to the other player. shadps4.exe sha256 starts $hash; both players must match."
