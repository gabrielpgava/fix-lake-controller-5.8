#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '..\scripts\LakeResourceMetrics.psm1') -Force
$checks = 0
function Assert($Value, $Name) {
    if (!$Value) { throw "FAIL: $Name" }; $script:checks++; Write-Host "PASS: $Name"
}
$args = @{HasExited=$false;ExpectedPid=42;ActualPid=42;ExpectedCreation=123;ActualCreation=123
    ExpectedPath='C:\synthetic.exe';ActualPath='c:\SYNTHETIC.exe';SampleCount=20;MaximumSamples=100
    ElapsedMilliseconds=20000;DurationMilliseconds=14400000}
Assert ($null -eq (Get-LakeObservationStopReason @args)) 'long observation continues with the same handle identity'
$args.ActualCreation = 124
Assert ((Get-LakeObservationStopReason @args) -eq 'TargetIdentityChanged') 'recycled PID stops instead of following the replacement'
$args.HasExited = $true
Assert ((Get-LakeObservationStopReason @args) -eq 'TargetExited') 'process exit takes precedence over a stale identity'
$args.HasExited = $false; $args.ActualCreation = 123; $args.SampleCount = 100
Assert ((Get-LakeObservationStopReason @args) -eq 'MaximumSamplesReached') 'sample bound caps output without ending the target'
$args.SampleCount = 20; $args.ElapsedMilliseconds = 14400001
Assert ((Get-LakeObservationStopReason @args) -eq 'DurationElapsed') 'delayed sample past duration is not fabricated'

# Exercise real script failure paths against this PowerShell process, never the
# admitted Lake. No bypass: its on-disk hash MUST be rejected by production code.
$sandbox = Join-Path (Split-Path $PSScriptRoot -Parent) ('private\monitor-policy-' + [guid]::NewGuid().ToString('N'))
$self = Get-Process -Id $PID
$rejected = $false
try {
    & (Join-Path $PSScriptRoot '..\scripts\Measure-LakeResources.ps1') -ProcessId $PID -ApplicationPath $self.Path `
        -DurationSeconds 14400 -MaximumSamples 100 -OutputDirectory $sandbox
} catch { $rejected = $true }
Assert $rejected 'four-hour duration is accepted but an unadmitted image is rejected before sampling'
$summary = Get-Content -Raw -LiteralPath (Get-ChildItem -LiteralPath $sandbox -Filter '*.json').FullName | ConvertFrom-Json
Assert ($summary.StopReason -eq 'ValidationFailed' -and $summary.RequestedDurationSeconds -eq 14400 -and
        $summary.Metrics.SampleCount -eq 0) 'real rejection summary distinguishes no samples from valid zero resources'
Assert ($null -ne (Get-Process -Id $PID)) 'failed monitor leaves its observed process alive'
$blocked = Join-Path $sandbox 'not-a-directory'
[IO.File]::WriteAllText($blocked,'foreign')
$failed = $false
try { & (Join-Path $PSScriptRoot '..\scripts\Measure-LakeResources.ps1') -ProcessId $PID -OutputDirectory $blocked }
catch { $failed = $true }
Assert ($failed -and [IO.File]::ReadAllText($blocked) -eq 'foreign') 'output failure preserves an existing foreign file'
Write-Host "$checks monitor policy checks; no Lake execution or termination."
