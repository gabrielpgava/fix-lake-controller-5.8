#Requires -Version 5.1
# Synthetic arithmetic/summary tests only. No process queries or Lake required.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Import-Module (Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts\LakeResourceMetrics.psm1') -Force -WarningAction Stop
$script:passed = 0
$script:failed = 0

function Assert-Equal {
    param([AllowNull()][object]$Actual, [AllowNull()][object]$Expected, [string]$Message)
    if ($Actual -ne $Expected) { throw "$Message (expected '$Expected', actual '$Actual')." }
}
function Assert-Null {
    param([AllowNull()][object]$Actual, [string]$Message)
    if ($null -ne $Actual) { throw "$Message (expected null, actual '$Actual')." }
}
function Assert-Near {
    param([AllowNull()][object]$Actual, [double]$Expected, [double]$Tolerance = 0.000000001)
    if ($null -eq $Actual -or [double]::IsNaN([double]$Actual) -or [double]::IsInfinity([double]$Actual) -or
        [math]::Abs([double]$Actual - $Expected) -gt $Tolerance) {
        throw "Expected finite $Expected +/- $Tolerance, actual '$Actual'."
    }
}
function Assert-InvalidCpu {
    param([object]$Actual, [string]$Reason)
    Assert-Equal $Actual.IsValid $false 'Invalid CPU must not be successful'
    Assert-Equal $Actual.InvalidReason $Reason 'Invalid CPU reason'
    foreach ($name in @('CpuDeltaMilliseconds', 'WallDeltaMilliseconds', 'CpuPercentOneCore', 'CpuPercentMachine')) {
        Assert-Null $Actual.$name "$name must be null for an invalid interval"
    }
}
function Get-TestSample {
    param(
        [AllowNull()][object]$Elapsed = 0, [AllowNull()][object]$Cpu = 0,
        [AllowNull()][object]$Private = 1000, [AllowNull()][object]$Resident = 2000,
        [AllowNull()][object]$Handles = 10, [AllowNull()][object]$Gdi = 4, [AllowNull()][object]$User = 5
    )
    return [pscustomobject]@{
        TimestampUtc = '2026-01-01T00:00:00.0000000Z'
        ElapsedMilliseconds = $Elapsed; TotalCpuMilliseconds = $Cpu
        PrivateBytes = $Private; WorkingSetBytes = $Resident
        HandleCount = $Handles; GdiObjects = $Gdi; UserObjects = $User
    }
}
function Invoke-Case {
    param([string]$Name, [scriptblock]$Body)
    try {
        & $Body
        $script:passed++
        Write-Host "PASS: $Name"
    } catch {
        $script:failed++
        Write-Host "FAIL: $Name - $($_.Exception.Message)"
    }
}

Invoke-Case 'CPU milliseconds / 1000ms wall / two logical CPUs' {
    $cpu = Get-LakeCpuMetrics 2000 3000 1000 2
    Assert-Equal $cpu.IsValid $true 'CPU validity'
    Assert-Null $cpu.InvalidReason 'Valid CPU has no failure reason'
    Assert-Near $cpu.CpuDeltaMilliseconds 1000
    Assert-Near $cpu.WallDeltaMilliseconds 1000
    Assert-Near $cpu.CpuPercentOneCore 100
    Assert-Near $cpu.CpuPercentMachine 50
}
Invoke-Case 'CPU uses actual elapsed milliseconds, not requested cadence' {
    $cpu = Get-LakeCpuMetrics 1000 2000 2000 2
    Assert-Near $cpu.CpuPercentOneCore 50
    Assert-Near $cpu.CpuPercentMachine 25
}
Invoke-Case 'CPU percentages are not clamped' {
    $cpu = Get-LakeCpuMetrics 0 6000 1000 2
    Assert-Equal $cpu.IsValid $true 'Uncapped arithmetic validity'
    Assert-Near $cpu.CpuPercentOneCore 600
    Assert-Near $cpu.CpuPercentMachine 300
}
Invoke-Case 'Zero CPU delta is valid idle time' {
    $cpu = Get-LakeCpuMetrics 1234 1234 1000 2
    Assert-Equal $cpu.IsValid $true 'Idle validity'
    Assert-Near $cpu.CpuDeltaMilliseconds 0
    Assert-Near $cpu.CpuPercentMachine 0
}
Invoke-Case 'Decreasing cumulative CPU is explicitly invalid' {
    Assert-InvalidCpu (Get-LakeCpuMetrics 2000 1999 1000 2) 'CpuCounterDecreased'
    $summary = Get-LakeResourceSummary -Samples @((Get-TestSample 0 2000), (Get-TestSample 1000 1999)) -LogicalProcessorCount 2
    Assert-Null $summary.TotalCpuMilliseconds.Delta 'Negative cumulative CPU delta is invalid'
    Assert-Equal $summary.CpuCumulativeDeltaValid $false 'Cumulative CPU validity'
}
Invoke-Case 'Zero, negative, missing and nonfinite wall intervals are invalid' {
    foreach ($wall in @(0, -1, $null, [double]::NaN, [double]::PositiveInfinity, [double]::NegativeInfinity, '1000')) {
        Assert-InvalidCpu (Get-LakeCpuMetrics 0 1000 $wall 2) 'InvalidElapsedMilliseconds'
    }
}
Invoke-Case 'Invalid CPU counters never become valid zeros' {
    foreach ($bad in @($null, -1, [double]::NaN, [double]::PositiveInfinity, [double]::NegativeInfinity, '0', $true)) {
        Assert-InvalidCpu (Get-LakeCpuMetrics $bad 1000 1000 2) 'InvalidPreviousCpuMilliseconds'
        Assert-InvalidCpu (Get-LakeCpuMetrics 0 $bad 1000 2) 'InvalidCurrentCpuMilliseconds'
    }
}
Invoke-Case 'Logical processor count must be a positive finite integer' {
    foreach ($count in @($null, 0, -1, 1.5, 1.0000000000000002, [double]::NaN, [double]::PositiveInfinity, '2', 2147483648L)) {
        Assert-InvalidCpu (Get-LakeCpuMetrics 0 1000 1000 $count) 'InvalidLogicalProcessorCount'
    }
}
Invoke-Case 'Floating CPU overflow is not reported as success' {
    Assert-InvalidCpu (Get-LakeCpuMetrics 0 ([double]::MaxValue) ([double]::Epsilon) 2) 'NonFiniteCpuResult'
}
Invoke-Case 'Floating CPU underflow is not reported as idle' {
    Assert-InvalidCpu (Get-LakeCpuMetrics 0 ([double]::Epsilon) ([double]::MaxValue) 2) 'UnrepresentableCpuResult'
}
Invoke-Case 'Small finite CPU intervals retain their units' {
    $cpu = Get-LakeCpuMetrics 0 0.000000001 0.000001 2
    Assert-Equal $cpu.IsValid $true 'Small interval validity'
    Assert-Near $cpu.CpuPercentOneCore 0.1
    Assert-Near $cpu.CpuPercentMachine 0.05
}
Invoke-Case 'Private bytes can grow while resident working set shrinks' {
    $samples = @((Get-TestSample 100 1000 1000 9000), (Get-TestSample 2100 2000 2000 4000))
    $summary = Get-LakeResourceSummary -Samples $samples -LogicalProcessorCount 2
    Assert-Equal $summary.SampleCount 2 'Sample count'
    Assert-Near $summary.ObservedMilliseconds 2000
    Assert-Equal $summary.PrivateBytes.Delta 1000 'Private delta'
    Assert-Equal $summary.WorkingSetBytes.Delta -5000 'Working set delta'
    Assert-Equal $summary.CpuValidIntervalCount 1 'Valid interval count'
    Assert-Near $summary.CpuPercentOneCore.End 50
    Assert-Near $summary.CpuPercentMachine.End 25
}
Invoke-Case 'Summary preserves start/end/min/max and observed count peaks' {
    $samples = @((Get-TestSample 0 0 100 900 3 10 2),
        (Get-TestSample 1000 200 500 1000 7 30 8), (Get-TestSample 2000 400 200 800 4 20 3))
    $summary = Get-LakeResourceSummary -Samples $samples -LogicalProcessorCount 2
    Assert-Equal $summary.PrivateBytes.Start 100 'Private start'
    Assert-Equal $summary.PrivateBytes.End 200 'Private end'
    Assert-Equal $summary.PrivateBytes.Min 100 'Private min'
    Assert-Equal $summary.PrivateBytes.Max 500 'Private observed peak'
    Assert-Equal $summary.PrivateBytes.Delta 100 'Private delta'
    Assert-Equal $summary.WorkingSetBytes.Min 800 'Resident min'
    Assert-Equal $summary.WorkingSetBytes.Max 1000 'Resident peak'
    Assert-Equal $summary.HandleCount.Max 7 'Handle peak'
    Assert-Equal $summary.HandleCount.Delta 1 'Handle delta'
    Assert-Equal $summary.GdiObjects.Max 30 'GDI peak'
    Assert-Equal $summary.UserObjects.Max 8 'USER peak'
}
Invoke-Case 'No samples means no invented endpoints or elapsed time' {
    $summary = Get-LakeResourceSummary -Samples @() -LogicalProcessorCount 2
    Assert-Equal $summary.SampleCount 0 'Empty sample count'
    Assert-Equal $summary.TimelineValid $false 'Empty timeline validity'
    Assert-Null $summary.ObservedMilliseconds 'Empty observed time'
    Assert-Equal $summary.PrivateBytes.ValidCount 0 'Empty metric count'
    foreach ($name in @('Start', 'End', 'Min', 'Max', 'Delta')) { Assert-Null $summary.PrivateBytes.$name "Empty $name" }
    Assert-Equal $summary.CpuValidIntervalCount 0 'Empty CPU interval count'
    Assert-Equal $summary.CpuInvalidIntervalCount 0 'No first-sample CPU failure invented'
}
Invoke-Case 'One sample has endpoints but no CPU interval' {
    $summary = Get-LakeResourceSummary -Samples @((Get-TestSample 15 1000)) -LogicalProcessorCount 2
    Assert-Equal $summary.TimelineValid $true 'Single timeline validity'
    Assert-Near $summary.ObservedMilliseconds 0
    Assert-Equal $summary.PrivateBytes.Delta 0 'Single-sample memory delta'
    Assert-Equal $summary.CpuPercentMachine.SampleCount 0 'No interval from one sample'
    Assert-Null $summary.CpuPercentMachine.End 'First CPU percent is absent'
}
Invoke-Case 'Unavailable counters retain null endpoints and explicit invalid counts' {
    $first = Get-TestSample 0 0 $null 2000 $null $null $null
    $second = Get-TestSample 1000 100 500 1500 0 0 0
    $summary = Get-LakeResourceSummary -Samples @($first, $second) -LogicalProcessorCount 2
    Assert-Null $summary.PrivateBytes.Start 'Missing first private bytes'
    Assert-Equal $summary.PrivateBytes.End 500 'Present last private bytes'
    Assert-Null $summary.PrivateBytes.Delta 'Do not bridge a missing endpoint'
    Assert-Equal $summary.HandleCount.InvalidCount 1 'Missing handle count'
    Assert-Equal $summary.HandleCount.ValidCount 1 'Known zero handle count is valid'
    Assert-Equal $summary.GdiObjects.End 0 'A supplied numeric zero is retained'
    $missing = Get-LakeResourceSummary -Samples @([pscustomobject]@{ ElapsedMilliseconds = 0; TotalCpuMilliseconds = 0 })
    Assert-Equal $missing.UserObjects.InvalidCount 1 'Absent optional property'
    Assert-Null $missing.UserObjects.Max 'Unavailable does not imply zero'
}
Invoke-Case 'Fractional, negative, string and nonfinite counters are invalid' {
    foreach ($bad in @(-1, 1.25, 0.9999999999999999, '100', [double]::NaN, [double]::PositiveInfinity,
        [double]::MaxValue, [double]9007199254740992L, [single]16777216)) {
        $summary = Get-LakeResourceSummary -Samples @((Get-TestSample -Private $bad))
        Assert-Equal $summary.PrivateBytes.InvalidCount 1 'Invalid byte counter count'
        Assert-Null $summary.PrivateBytes.Start 'Invalid byte counter endpoint'
        Assert-Null $summary.PrivateBytes.Delta 'Invalid byte counter delta'
    }
}
Invoke-Case 'UInt64 byte endpoints and one-byte deltas stay exact' {
    $high = [uint64]::MaxValue
    $low = [uint64]([decimal]$high - 1)
    $summary = Get-LakeResourceSummary -Samples @((Get-TestSample 0 0 $low), (Get-TestSample 1000 1 $high))
    Assert-Equal $summary.PrivateBytes.Start ([decimal]$low) 'Exact UInt64 start'
    Assert-Equal $summary.PrivateBytes.End ([decimal]$high) 'Exact UInt64 end'
    Assert-Equal $summary.PrivateBytes.Delta ([decimal]1) 'Exact single-byte delta'
    $boundary = Get-LakeResourceSummary -Samples @((Get-TestSample -Private ([double]9007199254740991L) -Resident ([single]16777215)))
    Assert-Equal $boundary.PrivateBytes.Start ([decimal]9007199254740991L) 'Exact double integer boundary'
    Assert-Equal $boundary.WorkingSetBytes.Start ([decimal]16777215) 'Exact single integer boundary'
}
Invoke-Case 'Non-increasing time and CPU resets invalidate intervals, not memory stats' {
    $samples = @((Get-TestSample 0 1000 100), (Get-TestSample 0 1100 200),
        (Get-TestSample 1000 900 300), (Get-TestSample 900 1000 400))
    $summary = Get-LakeResourceSummary -Samples $samples -LogicalProcessorCount 2
    Assert-Equal $summary.TimelineValid $false 'Invalid monotonic sequence'
    Assert-Null $summary.ObservedMilliseconds 'Do not invent elapsed time across invalid intervals'
    Assert-Equal $summary.CpuInvalidIntervalCount 3 'Invalid interval count'
    Assert-Equal $summary.CpuInvalidReasons['InvalidElapsedMilliseconds'] 2 'Invalid wall reasons'
    Assert-Equal $summary.CpuInvalidReasons['CpuCounterDecreased'] 1 'Reset reason'
    Assert-Null $summary.CpuPercentMachine.Max 'No valid CPU peak'
    Assert-Null $summary.TotalCpuMilliseconds.Delta 'Do not report a CPU total across invalid intervals'
    Assert-Equal $summary.PrivateBytes.Delta 300 'Independent memory endpoints'
}
Invoke-Case 'Streaming summary equals batch and does not mutate earlier state' {
    $samples = @((Get-TestSample 0 0 100), (Get-TestSample 1000 500 300), (Get-TestSample 1000 600 200))
    $first = Get-LakeResourceSummary -Samples @($samples[0]) -LogicalProcessorCount 2
    $before = ConvertTo-Json -InputObject $first -Depth 8 -Compress
    $next = Get-LakeResourceSummary -Samples @($samples[1]) -LogicalProcessorCount 2 -PreviousSummary $first
    $streamed = Get-LakeResourceSummary -Samples @($samples[2]) -LogicalProcessorCount 2 -PreviousSummary $next
    $batch = Get-LakeResourceSummary -Samples $samples -LogicalProcessorCount 2
    Assert-Equal (ConvertTo-Json -InputObject $first -Depth 8 -Compress) $before 'Prior state is immutable'
    Assert-Equal (ConvertTo-Json -InputObject $streamed -Depth 8 -Compress) `
        (ConvertTo-Json -InputObject $batch -Depth 8 -Compress) 'Streaming/batch summary'
    $rejected = $false
    try { Get-LakeResourceSummary -Samples @() -LogicalProcessorCount 4 -PreviousSummary $first | Out-Null }
    catch { $rejected = $true }
    Assert-Equal $rejected $true 'Do not silently change CPU normalization mid-run'
}
Invoke-Case 'Arithmetic and JSON stay numeric under pt-BR culture' {
    $thread = [Threading.Thread]::CurrentThread
    $oldCulture = $thread.CurrentCulture
    $oldUiCulture = $thread.CurrentUICulture
    try {
        $thread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('pt-BR')
        $thread.CurrentUICulture = [Globalization.CultureInfo]::GetCultureInfo('pt-BR')
        $cpu = Get-LakeCpuMetrics 0 1005 2000 2
        Assert-Near $cpu.CpuPercentMachine 25.125
        $json = ConvertTo-Json -InputObject $cpu -Compress
        # Valid JSON and numeric round-trip are the contract. Binary floating
        # arithmetic may serialize 25.125 as 25.124999999999996 on PS 5.1.
        $parsed = ConvertFrom-Json -InputObject $json
        Assert-Equal (($parsed.CpuPercentMachine -is [double]) -or
            ($parsed.CpuPercentMachine -is [decimal]) -or ($parsed.CpuPercentMachine -is [single])) $true 'JSON CPU percentage remains numeric'
        Assert-Near $parsed.CpuPercentMachine 25.125
        Assert-InvalidCpu (Get-LakeCpuMetrics 0 '1005,5' 2000 2) 'InvalidCurrentCpuMilliseconds'
        Assert-InvalidCpu (Get-LakeCpuMetrics 0 '1005.5' 2000 2) 'InvalidCurrentCpuMilliseconds'
        $summary = Get-LakeResourceSummary -Samples @((Get-TestSample 0 0 100), (Get-TestSample 2000 1005 200)) -LogicalProcessorCount 2
        Assert-Near $summary.CpuPercentMachine.End 25.125
        Assert-Equal $summary.PrivateBytes.Delta 100 'Culture-independent byte delta'
    } finally {
        $thread.CurrentCulture = $oldCulture
        $thread.CurrentUICulture = $oldUiCulture
    }
}
Invoke-Case 'Nonfinite sample time remains invalid in summaries' {
    $summary = Get-LakeResourceSummary -Samples @((Get-TestSample 0 0), (Get-TestSample ([double]::NaN) 100)) -LogicalProcessorCount 2
    Assert-Equal $summary.TimelineValid $false 'Nonfinite timeline'
    Assert-Null $summary.LastElapsedMilliseconds 'Invalid time is null'
    Assert-Null $summary.ObservedMilliseconds 'Invalid span is null'
    Assert-Equal $summary.CpuInvalidIntervalCount 1 'Invalid interval recorded'
    Assert-Equal $summary.CpuInvalidReasons['InvalidElapsedMilliseconds'] 1 'Nonfinite wall reason'
}

Write-Host "Resource metrics: $script:passed passed; total failures: $script:failed."
if ($script:failed -ne 0) { throw "Resource metrics tests failed: $script:failed." }
