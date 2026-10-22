# Prepares the release checkout: one checkout, <BB_ROOT>\release-src, that every release reuses
# (Kaniel 2026-10-22). A fresh checkout per release rebuilt the port, the server, the account tool
# and the emulator tests from nothing (about 11 of v1.1.2's 14.5 minutes). In the reused checkout
# they build incrementally, and the emulator tests run only when the emulator changed since the
# commit they last passed on (the release ships no emulator).
#
#   1. fetches, then moves the checkout to -Rev in place (jj new): files that did not change keep
#      their times, so ninja rebuilds only what changed. The first run makes the checkout (a jj
#      workspace of <BB_ROOT>\bloodborne-server) instead. A checkout with changes of its own stops it.
#   2. links the submodules (emulator\externals, bbport's third_party, server\externals) to the
#      folders -LinksFrom uses, like the release threads' links scripts did; fsr-vulkan, which
#      build.sh patches, is a clone of its own at the commit main pins instead (a shared one held
#      another version of the patches and the port did not build)
#   3. clears the CMake caches of the kept build folders that another checkout made, then says whether the emulator tests must run (tools\release\release_src.py emulator-check) and
#      prints the build folders to use, which stay between releases
#
# Usage: release-src.ps1 [-Rev main@origin] [-Root <dir>] [-LinksFrom <checkout>]
#        release-src.ps1 -MarkEmulatorTested     after the emulator tests passed in the checkout
param(
    [string]$Rev = "main@origin",
    [string]$Root,
    [string]$LinksFrom,
    [switch]$MarkEmulatorTested
)
$ErrorActionPreference = "Stop"
if (-not $env:BB_ROOT) { $env:BB_ROOT = "C:\bb" }
$default = Join-Path $env:BB_ROOT "release-src"
if (-not $Root) { $Root = $default }
if (-not $LinksFrom) { $LinksFrom = Join-Path $env:BB_ROOT "rel039-src" }
$repo = Join-Path $env:BB_ROOT "bloodborne-server"
$out = Join-Path $env:BB_ROOT "out"
$stamp = Join-Path $out "release-emu\tested-commit.txt"
$decide = Join-Path $PSScriptRoot "release\release_src.py"

if ($MarkEmulatorTested) {
    py -3.12 -I $decide emulator-mark --root $Root --stamp $stamp
    exit $LASTEXITCODE
}

# --- 1. the checkout at -Rev ---------------------------------------------------------------------
jj -R $repo git fetch
if ($LASTEXITCODE -ne 0) { throw "jj git fetch failed in $repo" }
if (-not (Test-Path (Join-Path $Root ".jj"))) {
    jj -R $repo workspace add --name (Split-Path $Root -Leaf) -r $Rev $Root
    if ($LASTEXITCODE -ne 0) { throw "jj workspace add $Root failed" }
} else {
    $own = jj -R $Root diff --summary -r "@"
    if ($own) { throw "$Root has changes of its own (a release builds only from a pushed commit):`n$($own -join "`n")" }
    jj -R $Root new $Rev
    if ($LASTEXITCODE -ne 0) { throw "jj new $Rev failed in $Root" }
}
$commit = jj -R $Root log -r "@-" --no-graph -T "commit_id"
Write-Host "$Root at $commit ($Rev)"

# --- 2. the submodules -------------------------------------------------------------------------
# build.sh patches fsr-vulkan's working tree (gpu\patches\fsr-vulkan), so it is not shared.
$own = "bbport\gpu\third_party\fsr-vulkan"
$links = cmd /c "dir /AL /S /B $LinksFrom\emulator\externals $LinksFrom\bbport $LinksFrom\server 2>nul" |
    Where-Object { $_ -notmatch '\\out\\' }
foreach ($link in $links) {
    $relative = $link.Substring($LinksFrom.Length + 1)
    if ($relative -eq $own) { continue }
    $target = (Get-Item $link).Target
    if ($target -is [array]) { $target = $target[0] }
    $here = Join-Path $Root $relative
    if (Test-Path $here) {
        $item = Get-Item $here -Force
        if ($item.LinkType) { continue }
        if ((Get-ChildItem $here -Force | Measure-Object).Count -gt 0) { Write-Host "  kept $relative (not empty)"; continue }
        [IO.Directory]::Delete($here)  # an empty submodule folder: the link takes its place
    }
    New-Item -ItemType Junction -Path $here -Target $target | Out-Null
    Write-Host "  linked $relative -> $target"
}

$pin = ((git -C $repo ls-tree $commit ($own -replace '\\', '/')) -split '\s+')[2]
if (-not $pin) { throw "no submodule commit for $own in $commit" }
$clone = Join-Path $Root $own
$upstream = "https://github.com/FireBurn/FSR-Vulkan.git"
if ((Get-Item $clone -Force -ErrorAction SilentlyContinue).LinkType) { cmd /c rmdir "$clone" }  # an older run's link
$fresh = $false
if (-not (Test-Path (Join-Path $clone ".git"))) {
    if (Test-Path $clone) { [IO.Directory]::Delete($clone) }  # the empty submodule folder
    # From the shared copy when there is one (no download), else from GitHub.
    $from = Get-Item (Join-Path $LinksFrom $own) -Force -ErrorAction SilentlyContinue
    $source = if ($from -and $from.LinkType) { @($from.Target)[0] } elseif ($from) { $from.FullName } else { $upstream }
    git clone -q --no-checkout $source $clone
    if ($LASTEXITCODE -ne 0) { git clone -q --no-checkout $upstream $clone }
    if ($LASTEXITCODE -ne 0) { throw "fsr-vulkan: cannot clone into $clone" }
    $fresh = $true
}
if ($fresh -or (git -C $clone rev-parse HEAD 2>$null) -ne $pin) {
    git -C $clone cat-file -e "$pin^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) { git -C $clone fetch -q $upstream $pin }
    git -C $clone checkout -q -f $pin
    if ($LASTEXITCODE -ne 0) { throw "fsr-vulkan: cannot check out $pin in $clone" }
    Write-Host "  $own at $pin (its own clone; build.sh applies the patches)"
}

# --- 3. what to run ---------------------------------------------------------------------------
# The kept build folders belong to the release checkout: another -Root (a test) leaves them alone.
if ($Root -eq $default) {
    $kept = "release-server", "release-clientsample", "release-emu\tests", "release-emu\build" | ForEach-Object { Join-Path $out $_ }
    py -3.12 -I $decide clear-stale-caches --root $Root @kept
    if ($LASTEXITCODE -ne 0) { throw "release_src.py clear-stale-caches failed" }
}
py -3.12 -I $decide emulator-check --root $Root --stamp $stamp
if ($LASTEXITCODE -ne 0) { throw "release_src.py emulator-check failed" }
Write-Host ""
Write-Host "Build folders (they stay, so builds are incremental):"
Write-Host "  port:          tools\build-bbport.ps1 -Src $Root\bbport -Out $out\release-bbport -Test"
Write-Host "  server:        tools\build-server.ps1 -Src $Root\server -Out $out\release-server   (make-release's own)"
Write-Host "  emulator tests: tools\build-emu.ps1 -Src $Root\emulator -Out $out\release-emu -TestsOnly, then -MarkEmulatorTested"
Write-Host "  release:       $Root\tools\make-release.ps1 -Version x.y.z -DlssDir $env:BB_ROOT\dlss-abi2 [-SkipPortBuild]"
