<#
.SYNOPSIS
    Run the native unit tests, targeted or full, without babysitting a 40-minute suite.

.DESCRIPTION
    The full native suite takes ~40 minutes because PlatformIO rebuilds a separate
    binary for each of the 35 test suites. Almost all development work only needs one
    or two of them.

    This script also exists to work around two environment problems that make
    test_schedules look broken when it is not:

      1. PlatformIO's native env needs a host gcc/g++. The one it can install itself
         (toolchain-gccmingw32) is 32-bit GCC 5.1 and cannot assemble this codebase -
         it dies on FakeIt with "too many sections". Set MINGW_BIN below to a 64-bit
         MinGW if it is not already on PATH.
      2. The native harness drives schedule ticks from a busy-wait spin thread that
         must wake every microsecond (lib/ArduinoFake/SoftwareTimer.cpp). Any CPU
         contention makes the *_accuracy_* tests overshoot and fail. This script kills
         stray PlatformIO/teensy_reboot zombies first, and warns if the machine is busy.

.EXAMPLE
    .\tools\run_tests.ps1 -Suites test_fuel,test_timers
    .\tools\run_tests.ps1                       # full suite, backgrounded, polled for you
    .\tools\run_tests.ps1 -Suites test_schedules -NoBuild
#>
[CmdletBinding()]
param(
    [string[]] $Suites = @(),
    [string]   $Env    = 'native_base',
    [string]   $MingwBin = 'C:\mingw64\bin',
    [switch]   $SkipPreflight,
    [switch]   $NoBuild
)

$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$pio   = 'C:\Users\laptop\.platformio\penv\Scripts\platformio.exe'
$log   = Join-Path $repo 'test_run.log'
$err   = Join-Path $repo 'test_run.err'

if (-not (Test-Path $pio)) { throw "PlatformIO not found at $pio" }

# Make sure a 64-bit host compiler is reachable.
if (Test-Path $MingwBin) { $env:PATH = "$MingwBin;$env:PATH" }
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    throw "No g++ on PATH. Install 64-bit MinGW (e.g. 'winget install BrechtSanders.WinLibs.POSIX.UCRT') and/or set -MingwBin."
}

if (-not $SkipPreflight) {
    # Zombie PlatformIO flashes and stray compiler processes steal the cores the
    # spin-thread timer needs. This is the single biggest cause of test_schedules
    # "failures", so clear them out before measuring anything timing-sensitive.
    Get-Process -Name teensy_reboot,python,platformio,cc1plus,g++ -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $PID } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2

    $sampleMs = 2000
    $a = (Get-Process | Measure-Object -Property CPU -Sum).Sum
    Start-Sleep -Milliseconds $sampleMs
    $b = (Get-Process | Measure-Object -Property CPU -Sum).Sum
    $cores  = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
    $busyPc = [math]::Round((($b - $a) / ($sampleMs / 1000.0) / $cores) * 100, 1)
    Write-Host "CPU busy: $busyPc%  ($cores logical cores)"
    if ($busyPc -gt 40) {
        Write-Warning "Machine is busy. test_accuracy_duration/timeout assert wall-clock timing within +/-600us and will likely produce false failures."
    }
}

$pioArgs = @('test', '-e', $Env)
foreach ($s in $Suites) { $pioArgs += @('-f', $s) }

# Run in the background: a full run exceeds a normal shell's patience, and there is no
# need to sit and watch it.
Remove-Item $log, $err -ErrorAction SilentlyContinue
$scope = if ($Suites.Count) { $Suites -join ',' } else { 'FULL' }
Write-Host "Running $scope in env '$Env' ... (log: $log)"
$proc = Start-Process -FilePath $pio -ArgumentList $pioArgs -WorkingDirectory $repo `
                      -RedirectStandardOutput $log -RedirectStandardError $err -NoNewWindow -PassThru

$lastReport = 0
while (-not $proc.HasExited) {
    Start-Sleep -Seconds 15
    $done = (Select-String -Path $log -Pattern '\[(PASSED|FAILED|ERRORED)\] Took' -ErrorAction SilentlyContinue | Measure-Object).Count
    if ($done -ne $lastReport) { Write-Host "  suites complete: $done"; $lastReport = $done }
}
$proc.WaitForExit()

# Report. Note that a non-zero exit does not necessarily mean the code is wrong - see the
# timing-tolerance warning above.
$failures = Select-String -Path $log -Pattern 'FAIL:' -ErrorAction SilentlyContinue
$summary  = Select-String -Path $log -Pattern 'test cases:' -ErrorAction SilentlyContinue

Write-Host ''
if ($summary) { $summary | ForEach-Object { Write-Host $_.Line } }
if ($failures) {
    Write-Host ''
    Write-Host "FAILURES ($($failures.Count)):" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $($_.Line)" }
} elseif ($summary) {
    Write-Host 'No assertion failures.' -ForegroundColor Green
}
if ($proc.ExitCode -ne 0 -and -not $failures) {
    $errored = Select-String -Path $err -Pattern 'Program errored|code$' -ErrorAction SilentlyContinue
    if ($errored) {
        Write-Host ''
        Write-Host "Note: a suite crashed in the harness rather than failing an assertion:" -ForegroundColor Yellow
        $errored | Select-Object -First 3 | ForEach-Object { Write-Host "  $($_.Line)" }
    }
}
exit $proc.ExitCode
