# Builds the native port (Bloodborne PC, bbport\ in this repository) with MSYS2 CLANG64, and stages
# what the rig plays with in <Out>: run.py with its scripts and patches, and the build in bin\
# (tools\rig\engines.py). The DLLs come from MSYS2 (run.py puts C:\msys64\clang64\bin on PATH).
# Usage: build-bbport.ps1 [-Src <bbport dir>] [-Out <dir>] [-Test]
#   -Test also builds and runs the port's co-op tests (guest code hooks, the rig's log file), the
#   renderer's buffer format check, the runtime's direct memory chunks and the game code hooks of
#   the PC memory model, the replacement of save data files, the party HUD's layout and its
#   reading of the game's HUD art, and the settings file (the game menu's FPS limit and DLSS model).
param(
    [string]$Src,
    [string]$Out,
    [switch]$Test
)
. "$PSScriptRoot\build-lock.ps1"
Invoke-UnderBuildLock $PSCommandPath $PSBoundParameters "build-bbport"
$ErrorActionPreference = "Stop"
if (-not $env:BB_ROOT) { $env:BB_ROOT = "C:\bb" }
if (-not $Src) { $Src = Join-Path $env:BB_ROOT "bloodborne-server\bbport" }
if (-not $Out) { $Out = Join-Path $env:BB_ROOT "out\bbport" }
$msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "C:\msys64" }
$bash = Join-Path $msys "usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    throw "No MSYS2 at $msys (winget install MSYS2.MSYS2, then the packages in bbport\packaging\windows\README.md)"
}

# A clean CLANG64 shell: the MSVC dev shell's settings (env.ps1) are not for this toolchain.
foreach ($name in "INCLUDE", "LIB", "LIBPATH", "CMAKE_PREFIX_PATH") { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
$env:MSYSTEM = "CLANG64"
$env:CHERE_INVOKING = "1"
$unix = (& (Join-Path $msys "usr\bin\cygpath.exe") -u $Src).Trim()
& $bash -lc "cd '$unix' && bash build.sh"
if ($LASTEXITCODE -ne 0) { Write-Host "bbport build failed"; exit 1 }
if ($Test) {
    & $bash -lc "cd '$unix' && source ./msys2-env.sh && ninja -C out/gpu coop-hooks-test log-file-test pixel-format-test vertex-formats-test depth-stencil-test memory-chunks-test guest-hooks-test savedata-replace-test save-editor-test party-layout-test key-guide-test party-icons-test coop-art-test portraits-test game-art-test settings-test texture-gc-test mouse-camera-test spec-drop-test cache-consistency-test cache-storage-test save-commit-test runtime-windows-test frame-slots-test flip-label-test flip-marker-test async-claim-test srt-fault-test push-data-test trade-rows-test pad-rebind-test screen-lighting-test output-sizes-test dlss-modes-test ui-composition-test prompts-test >out/test-build.log 2>&1 || { tail -40 out/test-build.log; exit 1; }; out/gpu/coop-hooks-test.exe && out/gpu/log-file-test.exe && out/gpu/pixel-format-test.exe && out/gpu/vertex-formats-test.exe && out/gpu/depth-stencil-test.exe && out/gpu/memory-chunks-test.exe >out/memory-chunks-test.log && tail -1 out/memory-chunks-test.log && out/gpu/guest-hooks-test.exe >out/guest-hooks-test.log && out/gpu/guest-hooks-test.exe traps >>out/guest-hooks-test.log && out/gpu/guest-hooks-test.exe crowded >>out/guest-hooks-test.log && grep ': ok' out/guest-hooks-test.log && out/gpu/savedata-replace-test.exe && out/gpu/save-editor-test.exe && out/gpu/party-layout-test.exe && out/gpu/key-guide-test.exe && out/gpu/party-icons-test.exe && out/gpu/coop-art-test.exe && out/gpu/portraits-test.exe && out/gpu/game-art-test.exe && out/gpu/settings-test.exe && out/gpu/texture-gc-test.exe && out/gpu/mouse-camera-test.exe && out/gpu/spec-drop-test.exe && out/gpu/cache-consistency-test.exe && out/gpu/cache-storage-test.exe && out/gpu/save-commit-test.exe && out/gpu/runtime-windows-test.exe && out/gpu/frame-slots-test.exe && out/gpu/flip-label-test.exe && out/gpu/flip-marker-test.exe && out/gpu/async-claim-test.exe && out/gpu/srt-fault-test.exe && out/gpu/push-data-test.exe && out/gpu/trade-rows-test.exe && out/gpu/pad-rebind-test.exe && out/gpu/screen-lighting-test.exe && out/gpu/output-sizes-test.exe && out/gpu/dlss-modes-test.exe && out/gpu/ui-composition-test.exe && out/gpu/prompts-test.exe"
    if ($LASTEXITCODE -ne 0) { Write-Host "bbport tests failed"; exit 1 }
}

# What the rig runs, apart from the source tree (a rebuild while games run waits for them).
New-Item -ItemType Directory -Force (Join-Path $Out "bin") | Out-Null
Copy-Item (Join-Path $Src "out\bb-probe.exe"), (Join-Path $Src "out\bb-gpu-capabilities.exe") (Join-Path $Out "bin") -Force
Copy-Item (Join-Path $Src "run.py") $Out -Force
foreach ($dir in "scripts", "patches") {
    robocopy (Join-Path $Src $dir) (Join-Path $Out $dir) /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Write-Host "copying $dir failed"; exit 1 }
}
# Legacy CPU (Intel 3rd generation and older): the plan scripts\cpu_compat.py applies.
New-Item -ItemType Directory -Force (Join-Path $Out "tools") | Out-Null
Copy-Item (Join-Path $Src "tools\legacy-cpu-plan.json") (Join-Path $Out "tools") -Force
Write-Host "Port staged in $Out"
exit 0
