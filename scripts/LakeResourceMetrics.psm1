# Pure arithmetic only: importing this module never queries a process or the OS.
# Samples use cumulative CPU milliseconds and monotonic elapsed milliseconds.
Set-StrictMode -Version 2.0

function Get-LakeMetricNumber {
    param([AllowNull()][object]$Value, [switch]$Counter)

    # Do not interpret strings according to the caller's current culture, or
    # silently turn null/bool into zero. Native counters arrive as CLR numbers.
    if ($null -eq $Value -or -not (
        $Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64] -or
        $Value -is [single] -or $Value -is [double] -or $Value -is [decimal])) {
        return $null
    }
    $number = [double]$Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt 0) {
        return $null
    }
    if ($Counter) {
        # Preserve integer byte counts (including UInt64) exactly in summaries.
        # Floating counters beyond their exact-integer range are not trustworthy.
        if (($Value -is [double] -and $number -gt 9007199254740991.0) -or
            ($Value -is [single] -and $number -gt 16777215.0)) { return $null }
        if (($Value -is [double] -or $Value -is [single]) -and [math]::Truncate($number) -ne $number) {
            return $null
        }
        try {
            if ($Value -is [double] -or $Value -is [single]) {
                # A direct floating-to-decimal conversion can round to 7/15
                # significant digits even for an exactly represented integer.
                $integer = [decimal]([uint64]$Value)
            } else { $integer = [decimal]$Value }
        } catch { return $null }
        if ($integer -gt [decimal][uint64]::MaxValue -or
            [decimal]::Truncate($integer) -ne $integer) { return $null }
        return $integer
    }
    return $number
}

function Get-LakeCpuMetrics {
    <#
    CPU percentages are not capped: 100 means one fully occupied logical CPU.
    CpuPercentMachine divides that value by LogicalProcessorCount. Invalid
    inputs leave ALL interval results null, with an explicit reason. A zero CPU
    delta is valid; a decreasing counter or nonpositive wall interval is not.
    #>
    [CmdletBinding()]
    param(
        [AllowNull()][object]$PreviousCpuMilliseconds,
        [AllowNull()][object]$CurrentCpuMilliseconds,
        [AllowNull()][object]$ElapsedMilliseconds,
        [AllowNull()][object]$LogicalProcessorCount
    )
    $result = [ordered]@{
        IsValid = $false
        InvalidReason = $null
        CpuDeltaMilliseconds = $null
        WallDeltaMilliseconds = $null
        CpuPercentOneCore = $null
        CpuPercentMachine = $null
    }
    $previous = Get-LakeMetricNumber $PreviousCpuMilliseconds
    $current = Get-LakeMetricNumber $CurrentCpuMilliseconds
    $elapsed = Get-LakeMetricNumber $ElapsedMilliseconds
    $processors = Get-LakeMetricNumber $LogicalProcessorCount -Counter
    if ($null -eq $previous) { $result.InvalidReason = 'InvalidPreviousCpuMilliseconds' }
    elseif ($null -eq $current) { $result.InvalidReason = 'InvalidCurrentCpuMilliseconds' }
    elseif ($null -eq $elapsed -or $elapsed -le 0) { $result.InvalidReason = 'InvalidElapsedMilliseconds' }
    elseif ($null -eq $processors -or $processors -lt 1 -or $processors -gt [int]::MaxValue) {
        $result.InvalidReason = 'InvalidLogicalProcessorCount'
    }
    elseif ($current -lt $previous) { $result.InvalidReason = 'CpuCounterDecreased' }
    else {
        $delta = $current - $previous
        $oneCore = ($delta / $elapsed) * 100.0
        $machine = $oneCore / [double]$processors
        if ([double]::IsNaN($oneCore) -or [double]::IsInfinity($oneCore) -or
            [double]::IsNaN($machine) -or [double]::IsInfinity($machine)) {
            $result.InvalidReason = 'NonFiniteCpuResult'
        } elseif ($delta -gt 0 -and ($oneCore -eq 0 -or $machine -eq 0)) {
            $result.InvalidReason = 'UnrepresentableCpuResult'
        } else {
            $result.IsValid = $true
            $result.CpuDeltaMilliseconds = $delta
            $result.WallDeltaMilliseconds = $elapsed
            $result.CpuPercentOneCore = $oneCore
            $result.CpuPercentMachine = $machine
        }
    }
    return [pscustomobject]$result
}

function Get-LakeEmptyStatistic {
    return [pscustomobject][ordered]@{
        SampleCount = [long]0; ValidCount = [long]0; InvalidCount = [long]0
        Start = $null; End = $null; Min = $null; Max = $null; Delta = $null
    }
}

function Get-LakeStatistic {
    param([object]$State, [AllowNull()][object]$Value, [switch]$Counter)
    $number = Get-LakeMetricNumber $Value -Counter:$Counter
    $start = $State.Start
    if ($State.SampleCount -eq 0) { $start = $number }
    $minimum = $State.Min
    $maximum = $State.Max
    $validCount = $State.ValidCount
    if ($null -ne $number) {
        $validCount++
        if ($null -eq $minimum -or $number -lt $minimum) { $minimum = $number }
        if ($null -eq $maximum -or $number -gt $maximum) { $maximum = $number }
    }
    $delta = $null
    if ($null -ne $start -and $null -ne $number) { $delta = $number - $start }
    return [pscustomobject][ordered]@{
        SampleCount = [long]($State.SampleCount + 1)
        ValidCount = [long]$validCount
        InvalidCount = [long]($State.SampleCount + 1 - $validCount)
        Start = $start; End = $number; Min = $minimum; Max = $maximum; Delta = $delta
    }
}

function Get-LakeSampleValue {
    param([AllowNull()][object]$Sample, [string]$Name)
    if ($null -eq $Sample) { return $null }
    if ($Sample -is [System.Collections.IDictionary]) { return $Sample[$Name] }
    $property = $Sample.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-LakeResourceSummary {
    <#
    Each metric reports actual first/last endpoints and valid observed min/max.
    A missing endpoint keeps Delta null; it is never replaced with zero or an
    earlier valid sample. Counter deltas may be negative (notably working set).

    PreviousSummary enables constant-space streaming. This function returns new
    objects and does not mutate that summary or the supplied samples. Supplying
    all samples at once produces the same result. Keep LogicalProcessorCount
    unchanged across updates. No rate/leak diagnosis or hours-long extrapolation
    is inferred from a finite observation window.
    #>
    [CmdletBinding()]
    param(
        [AllowEmptyCollection()][AllowNull()][object[]]$Samples = @(),
        [AllowNull()][object]$LogicalProcessorCount = 1,
        [AllowNull()][object]$PreviousSummary
    )
    $processors = Get-LakeMetricNumber $LogicalProcessorCount -Counter
    if ($null -ne $processors -and $processors -ge 1 -and $processors -le [int]::MaxValue) {
        $processors = [int]$processors
    } else { $processors = $null }

    $counterNames = @('PrivateBytes', 'WorkingSetBytes', 'HandleCount', 'GdiObjects', 'UserObjects')
    $intervalNames = @('CpuDeltaMilliseconds', 'WallDeltaMilliseconds', 'CpuPercentOneCore', 'CpuPercentMachine')
    if ($null -eq $PreviousSummary) {
        $state = [ordered]@{
            SchemaVersion = 1
            LogicalProcessorCount = $processors
            SampleCount = [long]0
            FirstSampleUtc = $null; LastSampleUtc = $null
            FirstElapsedMilliseconds = $null; LastElapsedMilliseconds = $null
            LastTotalCpuMilliseconds = $null
            TimelineValid = $false; ObservedMilliseconds = $null
            CpuValidIntervalCount = [long]0; CpuInvalidIntervalCount = [long]0
            CpuCumulativeDeltaValid = $false
            CpuInvalidReasons = [ordered]@{}
        }
        foreach ($name in ($counterNames + @('TotalCpuMilliseconds') + $intervalNames)) {
            $state[$name] = Get-LakeEmptyStatistic
        }
    } else {
        if ($PreviousSummary.SchemaVersion -ne 1 -or $PreviousSummary.LogicalProcessorCount -ne $processors) {
            throw 'PreviousSummary must use schema 1 and the same LogicalProcessorCount.'
        }
        $state = [ordered]@{}
        foreach ($property in $PreviousSummary.PSObject.Properties) { $state[$property.Name] = $property.Value }
        $state.CpuInvalidReasons = [ordered]@{}
        foreach ($key in $PreviousSummary.CpuInvalidReasons.Keys) {
            $state.CpuInvalidReasons[$key] = $PreviousSummary.CpuInvalidReasons[$key]
        }
    }

    foreach ($sample in $Samples) {
        $elapsed = Get-LakeMetricNumber (Get-LakeSampleValue $sample 'ElapsedMilliseconds')
        $cpu = Get-LakeMetricNumber (Get-LakeSampleValue $sample 'TotalCpuMilliseconds')
        $timestamp = Get-LakeSampleValue $sample 'TimestampUtc'
        if ($state.SampleCount -eq 0) {
            $state.FirstSampleUtc = $timestamp
            $state.FirstElapsedMilliseconds = $elapsed
            $state.TimelineValid = $null -ne $elapsed
        } else {
            $wall = $null
            if ($null -ne $elapsed -and $null -ne $state.LastElapsedMilliseconds) {
                $wall = $elapsed - $state.LastElapsedMilliseconds
            }
            if ($null -eq $wall -or $wall -le 0) { $state.TimelineValid = $false }
            $interval = Get-LakeCpuMetrics -PreviousCpuMilliseconds $state.LastTotalCpuMilliseconds `
                -CurrentCpuMilliseconds $cpu -ElapsedMilliseconds $wall -LogicalProcessorCount $processors
            if ($interval.IsValid) { $state.CpuValidIntervalCount++ }
            else {
                $state.CpuInvalidIntervalCount++
                $reason = $interval.InvalidReason
                if (-not $state.CpuInvalidReasons.Contains($reason)) { $state.CpuInvalidReasons[$reason] = [long]0 }
                $state.CpuInvalidReasons[$reason]++
            }
            foreach ($name in $intervalNames) {
                $state[$name] = Get-LakeStatistic -State $state[$name] -Value $interval.$name
            }
        }
        foreach ($name in $counterNames) {
            $state[$name] = Get-LakeStatistic -State $state[$name] -Value (Get-LakeSampleValue $sample $name) -Counter
        }
        $state.TotalCpuMilliseconds = Get-LakeStatistic -State $state.TotalCpuMilliseconds -Value $cpu
        $state.CpuCumulativeDeltaValid = $state.CpuInvalidIntervalCount -eq 0 -and
            $null -ne $state.TotalCpuMilliseconds.Start -and $null -ne $state.TotalCpuMilliseconds.End
        if (-not $state.CpuCumulativeDeltaValid) { $state.TotalCpuMilliseconds.Delta = $null }
        $state.SampleCount++
        $state.LastSampleUtc = $timestamp
        $state.LastElapsedMilliseconds = $elapsed
        $state.LastTotalCpuMilliseconds = $cpu
        $state.ObservedMilliseconds = $null
        if ($state.TimelineValid) { $state.ObservedMilliseconds = $elapsed - $state.FirstElapsedMilliseconds }
    }
    return [pscustomobject]$state
}

Export-ModuleMember -Function Get-LakeCpuMetrics, Get-LakeResourceSummary
