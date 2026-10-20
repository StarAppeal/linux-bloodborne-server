# Builds the emulator the way upstream CI does: the emulator (tests off) and the unit tests
# (ENABLE_TESTS=ON, which skips the emulator target) in separate build dirs, outside the source
# tree so REUSE lint and jj never see build output.
param(
    [string]$Type = "RelWithDebInfo",
    [string]$Src,
    [string]$Out,
    [switch]$TestsOnly
)
. "$PSScriptRoot\build-lock.ps1"
Invoke-UnderBuildLock $PSCommandPath $PSBoundParameters "build-emu"
. "$PSScriptRoot\env.ps1"
if (-not $Src) { $Src = Join-Path $env:BB_ROOT "bloodborne-server\emulator" }
if (-not $Out) { $Out = Join-Path $env:BB_ROOT "out\emulator" }
$common = @("-S", $Src, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Type",
            "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl")
$failed = $false
$dirs = @(@{ Dir = "tests"; Tests = "ON" })
if (-not $TestsOnly) { $dirs = @(@{ Dir = "build"; Tests = "OFF" }) + $dirs }
foreach ($d in $dirs) {
    $bin = Join-Path $Out $d.Dir
    cmake -B $bin @common "-DENABLE_TESTS=$($d.Tests)" 2>&1 | Select-Object -Last 1
    $log = Join-Path $Out "$($d.Dir).log"
    cmake --build $bin --parallel 24 *> $log
    if ($LASTEXITCODE -ne 0) {
        $failed = $true
        Write-Host "BUILD FAILED in $bin (log: $log)"
        Select-String -Path $log -Pattern "error:|FAILED" | Select-Object -First 20 | ForEach-Object Line
    }
}
Get-ChildItem (Join-Path $Out "build\shadPS4.exe") -ErrorAction SilentlyContinue | Select-Object FullName, Length
if ($failed) { exit 1 }
