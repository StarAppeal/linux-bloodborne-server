# Asks the player for their settings, then prepares this kit's portable `user` dir:
#   - copies DATA\user from their Bloodborne.PC.Online install the first time (their saves, keys, input)
#   - points shadnet, the WebAPI and the ss4 backend at the host's IP
#   - puts their shadnet account into users.json
# Answers are kept in settings.bat, which PLAY.bat and the HOST bats read.
$ErrorActionPreference = "Stop"
$kit = Split-Path -Parent $PSScriptRoot
$settingsPath = Join-Path $kit "settings.bat"

# shadPS4 reads these with a JSON parser; write UTF-8 without the BOM Windows PowerShell adds.
function Write-Utf8([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding $false)) }
function Set-Prop($Obj, [string]$Name, $Value) { $Obj | Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force }

$saved = @{}
if (Test-Path $settingsPath) {
    foreach ($line in Get-Content $settingsPath) {
        if ($line -match '^set "([A-Z0-9_]+)=(.*)"$') { $saved[$Matches[1]] = $Matches[2] }
    }
}

function Ask([string]$Prompt, [string]$Default, [scriptblock]$Valid, [string]$Hint) {
    while ($true) {
        $shown = if ($Default) { " [$Default]" } else { "" }
        $answer = (Read-Host "  $Prompt$shown").Trim()
        if ($answer -eq "") { $answer = $Default }
        if ($answer -and (& $Valid $answer)) { return $answer }
        Write-Host "    $Hint" -ForegroundColor Red
    }
}

# Not loopback: shadnet tells the other player the address it sees you on, and 127.0.0.1 means nothing to them.
$isIp = { param($v) $ip = $null; ($v -match '^\d{1,3}(\.\d{1,3}){3}$') -and [Net.IPAddress]::TryParse($v, [ref]$ip) -and $v -notmatch '^127\.' }
$isName = { param($v) $v -match '^[A-Za-z0-9_-]{3,16}$' }
$isSecret = { param($v) $v -match '^[A-Za-z0-9_-]{1,32}$' }
$isParty = { param($v) $v -match '^[A-Za-z0-9_-]{1,32}:[A-Za-z0-9_-]{1,32}$' }
$isRole = { param($v) $v -eq "host" -or $v -eq "guest" }
$isInstall = { param($v) Test-Path (Join-Path $v "DATA\Bloodborne\CUSA03173\eboot.bin") }
$isPort = { param($v) ($v -match '^\d{4,5}$') -and [int]$v -ge 1024 -and [int]$v -le 65535 }

Write-Host ""
Write-Host "  Bloodborne seamless co-op test: setup"
Write-Host "  ====================================="
Write-Host "  Press Enter to keep the value in [brackets]."
Write-Host ""

$defInstall = $saved["INSTALL"]
if (-not $defInstall) {
    foreach ($c in @((Split-Path -Parent $kit), "C:\Bloodborne.PC.Online")) {
        if (& $isInstall $c) { $defInstall = $c; break }
    }
}
$install = Ask "Your Bloodborne.PC.Online folder" $defInstall $isInstall "No DATA\Bloodborne\CUSA03173\eboot.bin in that folder."
$role = Ask "Are you the host or the guest? (host/guest)" $saved["ROLE"] $isRole "Type host or guest."
Write-Host ""
Write-Host "  The host's IP is the one shown in Radmin VPN (or Tailscale) on the HOST's PC."
Write-Host "  The host types their own VPN IP here too, never 127.0.0.1."
$hostIp = Ask "Host IP" $saved["HOSTIP"] $isIp "Enter an IPv4 address like 26.12.34.56."
Write-Host ""
Write-Host "  Your shadnet account. The host registers it with 'HOST - Register Account'."
$npid = Ask "Account name (3-16 letters, digits, _ or -)" $saved["NPID"] $isName "3-16 characters: letters, digits, _ or -."
$pass = Ask "Account password (letters, digits, _ or -)" $saved["PASS"] $isSecret "1-32 characters: letters, digits, _ or -."
Write-Host ""
Write-Host "  The party is name:secret, and both players must type exactly the same thing."
$party = Ask "Party" $(if ($saved["PARTY"]) { $saved["PARTY"] } else { "hunters:changeme" }) $isParty "Use name:secret, letters, digits, _ or - only."
$defPort = if ($saved["P2PPORT"]) { $saved["P2PPORT"] } elseif ($role -eq "host") { "51610" } else { "51611" }
$p2pPort = Ask "P2P UDP port" $defPort $isPort "A number from 1024 to 65535."

# --- user dir --------------------------------------------------------------------------------
$user = Join-Path $kit "user"
if (-not (Test-Path (Join-Path $user "custom_configs\CUSA03173.json"))) {
    Write-Host ""
    Write-Host "  Copying your settings and saves from $install\DATA\user ..."
    robocopy "$install\DATA\user" $user /E /XD log shader cache captures screenshots temp /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
}

# --- game folder -----------------------------------------------------------------------------
# kit\game is junctions to the install's base game, patch and active mods, plus a real "event"
# folder: the install's event mods with the kit's patched map events on top. shadPS4 takes the
# -patch and -mods folders from the -g path, so the install itself is never changed.
$src = "$install\DATA\Bloodborne"
$gameDir = Join-Path $kit "game"
$modsDst = "$gameDir\CUSA03173-mods\dvdroot_ps4"
function Remove-Junction([string]$Path) {
    # rmdir removes only the link; Remove-Item -Recurse could follow it into the install.
    if (Test-Path $Path) { cmd /c rmdir "$Path" | Out-Null }
}
New-Item -ItemType Directory -Force $modsDst | Out-Null
foreach ($name in "CUSA03173", "CUSA03173-patch") {
    Remove-Junction "$gameDir\$name"
    New-Item -ItemType Junction -Path "$gameDir\$name" -Target "$src\$name" | Out-Null
}
foreach ($item in Get-ChildItem -Force $modsDst) {
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { Remove-Junction $item.FullName }
    else { Remove-Item -Recurse -Force $item.FullName }
}
$skip = @("MODS", "!BACKUP", "!INSTLOGS", "JSGME.exe", "JSGME.ini", "event")
foreach ($item in Get-ChildItem -Force "$src\CUSA03173-mods\dvdroot_ps4" | Where-Object { $_.Name -notin $skip }) {
    if ($item.PSIsContainer) { New-Item -ItemType Junction -Path "$modsDst\$($item.Name)" -Target $item.FullName | Out-Null }
    else { Copy-Item $item.FullName "$modsDst\" }
}
New-Item -ItemType Directory -Force "$modsDst\event" | Out-Null
if (Test-Path "$src\CUSA03173-mods\dvdroot_ps4\event") { Copy-Item "$src\CUSA03173-mods\dvdroot_ps4\event\*" "$modsDst\event\" -Recurse -Force }
Copy-Item "$kit\event-patch\*" "$modsDst\event\" -Force

$cfgPath = Join-Path $user "custom_configs\CUSA03173.json"
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
Set-Prop $cfg.General "connected_to_network" $true
Set-Prop $cfg.General "shad_net_enabled" $true
Set-Prop $cfg.General "enable_upnp" $false
Set-Prop $cfg.General "shadnet_server" "${hostIp}:31313"
Set-Prop $cfg.General "shadnet_webapi_server" "http://${hostIp}:31315"
Write-Utf8 $cfgPath ($cfg | ConvertTo-Json -Depth 10)

$usersPath = Join-Path $user "users.json"
$users = Get-Content $usersPath -Raw | ConvertFrom-Json
$u = @($users.Users.user)[0]
Set-Prop $u "shadnet_enabled" $true
Set-Prop $u "shadnet_npid" $npid
Set-Prop $u "shadnet_password" $pass
Set-Prop $u "shadnet_token" ""
Set-Prop $u "user_name" $npid
Write-Utf8 $usersPath ($users | ConvertTo-Json -Depth 10)

$ovrPath = Join-Path $user "host_overrides.json"
if ((Test-Path $ovrPath) -and -not (Test-Path "$ovrPath.backup")) { Copy-Item $ovrPath "$ovrPath.backup" }
Write-Utf8 $ovrPath ("{`n  `"https://ss4.scej-network.jp:20443`": `"http://${hostIp}:20443`"`n}`n")

$lines = @(
    "@rem Written by 1 - SETUP.bat. Run it again to change these.",
    "set `"INSTALL=$install`"",
    "set `"ROLE=$role`"",
    "set `"HOSTIP=$hostIp`"",
    "set `"NPID=$npid`"",
    "set `"PASS=$pass`"",
    "set `"PARTY=$party`"",
    "set `"P2PPORT=$p2pPort`""
)
[IO.File]::WriteAllLines($settingsPath, $lines, (New-Object Text.ASCIIEncoding))

Write-Host ""
Write-Host "  Done." -ForegroundColor Green
Write-Host "    role $role, host $hostIp, account $npid, party $party, P2P UDP $p2pPort"
Write-Host "    shadnet ${hostIp}:31313, WebAPI ${hostIp}:31315, backend ${hostIp}:20443"
if ($role -eq "host") {
    Write-Host ""
    Write-Host "  Next: 'HOST - Start Servers', then 'HOST - Register Account' for you and your friend."
} else {
    Write-Host ""
    Write-Host "  Next: ask the host to register '$npid' with password '$pass', then run '2 - PLAY'."
}
