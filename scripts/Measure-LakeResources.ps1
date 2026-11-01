#Requires -Version 5.1
<#
.SYNOPSIS
Read-only, explicitly PID-bound observation of the supported Lake Build 007.
.DESCRIPTION
Does not start/stop/suspend Lake, inject code, read process memory or traffic,
enumerate windows, send input, or change any application/system configuration.
The script writes only new CSV/JSON files and their output directory; the
PowerShell Add-Type runtime may also use its own compiler/temp artifacts.
Image validation hashes the on-disk EXE, not the process's mapped code pages.
An open query/synchronize handle and creation time bind every sample to the
original process; a recycled PID is never reopened or followed.

Private bytes and working set are different counters. Growth in either is not
proof of a leak or its cause. Compare separately labelled, otherwise comparable
runs; do not linearly extrapolate a short observation to hours. Label is stored
only in JSON: use a generic, non-sensitive label, not app text/preset names.

CSV rows are flushed incrementally. Final JSON is best effort on interruption:
forced host termination, power loss, parameter errors, or an unwritable output
directory can prevent it. Ctrl+C normally runs finally; it never stops Lake.
.EXAMPLE
.\Measure-LakeResources.ps1 -ProcessId 1234 -DurationSeconds 1800 -Label baseline-idle
.EXAMPLE
.\Measure-LakeResources.ps1 -ProcessId 1234 -MaximumPrivateMiB 2048 -Label comparison-A
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateRange(1, 2147483647)][int]$ProcessId,
    [ValidateNotNullOrEmpty()][string]$ApplicationPath = 'C:\Program Files (x86)\Lake\Lake Controller v5.8\Lake Controller.exe',
    [ValidateScript({
        if ([double]::IsNaN($_) -or [double]::IsInfinity($_) -or $_ -lt 10 -or $_ -gt 86400) {
            throw 'DurationSeconds must be finite and between 10 and 86400 (24 hours).'
        }; $true
    })][double]$DurationSeconds = 300,
    [ValidateScript({
        if ([double]::IsNaN($_) -or [double]::IsInfinity($_) -or $_ -lt 0.1 -or $_ -gt 30) {
            throw 'IntervalSeconds must be finite and between 0.1 and 30.'
        }; $true
    })][double]$IntervalSeconds = 1,
    [AllowEmptyString()][ValidateLength(0, 120)][ValidateScript({
        if ($_ -match '[\x00-\x1f\x7f]') { throw 'Label must not contain control characters.' }; $true
    })][string]$Label = '',
    [string]$OutputDirectory,
    [ValidateScript({
        if ([double]::IsNaN($_) -or [double]::IsInfinity($_) -or $_ -lt 0 -or
            $_ -gt ([double][uint64]::MaxValue / 1048576.0)) {
            throw 'MaximumPrivateMiB must be a finite nonnegative counter threshold (0 disables it).'
        }; $true
    })][double]$MaximumPrivateMiB = 0,
    # Bound output even at the fastest cadence. Reaching this limit stops only
    # this observer; choose a larger interval for a long run, not an unbounded log.
    [ValidateRange(1, 1000000)][int]$MaximumSamples = 100000
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Get-LakeCsvLine {
    param([object[]]$Values)
    $fields = foreach ($value in $Values) {
        $text = ''
        if ($null -ne $value) {
            if ($value -is [double] -or $value -is [single]) {
                $text = $value.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
            } elseif ($value -is [System.IFormattable]) {
                $text = $value.ToString($null, [Globalization.CultureInfo]::InvariantCulture)
            } else { $text = [string]$value }
        }
        '"' + $text.Replace('"', '""') + '"'
    }
    return $fields -join ','
}

function Get-LakeFailureDetails {
    param([AllowNull()][System.Exception]$Exception)
    $type = $null
    $code = $null
    while ($null -ne $Exception) {
        $type = $Exception.GetType().FullName
        if ($Exception -is [System.ComponentModel.Win32Exception]) { $code = $Exception.NativeErrorCode }
        $Exception = $Exception.InnerException
    }
    # Do not persist arbitrary exception messages, paths of rejected processes,
    # command lines, window titles, or any other application-provided text.
    return [pscustomobject]@{ Type = $type; Win32ErrorCode = $code }
}

$expectedSha256 = 'e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713'
$invariant = [Globalization.CultureInfo]::InvariantCulture
$runStartedUtc = [datetime]::UtcNow
$runId = [guid]::NewGuid().ToString('N')
$csvPath = $null; $jsonPath = $null
$csvStream = $null; $csvWriter = $null; $jsonStream = $null; $jsonWriter = $null
$observer = $null; $metrics = $null; $failure = $null; $errorStage = $null
$logicalProcessorCount = $null; $validatedPath = $null; $validatedCreation = $null
$creationUtc = $null; $imageValidated = $false; $summaryWritten = $false
$observationStartedUtc = $null; $originTicks = $null; $observationElapsedMs = $null
$stopReason = 'Interrupted'
$stage = 'OutputInitialization'
$skippedSampleSlots = [long]0
$columns = @('SampleIndex', 'TimestampUtc', 'ElapsedMilliseconds', 'TotalCpuMilliseconds',
    'WallDeltaMilliseconds', 'CpuDeltaMilliseconds', 'CpuPercentOneCore', 'CpuPercentMachine',
    'CpuValid', 'CpuInvalidReason', 'PrivateBytes', 'WorkingSetBytes', 'HandleCount',
    'HandleCountStatus', 'GdiObjects', 'GdiStatus', 'UserObjects', 'UserStatus')

try {
    if (-not $PSBoundParameters.ContainsKey('OutputDirectory')) {
        # Source layout: project/scripts/. Packaged layout: scripts next to EXE.
        $baseDirectory = $PSScriptRoot
        if ([IO.Path]::GetFileName($PSScriptRoot) -ieq 'scripts') {
            $baseDirectory = Split-Path $PSScriptRoot -Parent
        }
        $OutputDirectory = Join-Path $baseDirectory 'private\measurements'
    }
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { throw 'OutputDirectory must not be empty.' }
    $outputProvider = $null; $outputDrive = $null
    $outputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $OutputDirectory, [ref]$outputProvider, [ref]$outputDrive)
    if ($outputProvider.Name -ne 'FileSystem') { throw 'OutputDirectory must use the filesystem provider.' }
    [IO.Directory]::CreateDirectory($outputPath) | Out-Null
    $stamp = $runStartedUtc.ToString("yyyyMMdd'T'HHmmssfffffff'Z'", $invariant)
    $stem = 'lake-resources-{0}-pid{1}-{2}' -f $stamp, $ProcessId, $runId
    $csvPath = Join-Path $outputPath ($stem + '.csv')
    $jsonPath = Join-Path $outputPath ($stem + '.json')
    # Reserve both names with CREATE_NEW: never truncate an existing artifact.
    $jsonStream = [IO.File]::Open($jsonPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $csvStream = [IO.File]::Open($csvPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $encoding = [Text.UTF8Encoding]::new($false)
    $csvWriter = [IO.StreamWriter]::new($csvStream, $encoding)
    $csvWriter.NewLine = "`r`n"
    $csvWriter.AutoFlush = $true
    $csvWriter.WriteLine((Get-LakeCsvLine $columns))

    $stage = 'PlatformValidation'
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'This monitor requires Windows.' }
    $stage = 'ModuleImport'
    Import-Module (Join-Path $PSScriptRoot 'LakeResourceMetrics.psm1') -Force -WarningAction Stop
    $metrics = Get-LakeResourceSummary -LogicalProcessorCount $null
    $stage = 'InteropInitialization'
    if (-not ('LakeResourceObservationV1.ObservedProcess' -as [type])) {
        Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace LakeResourceObservationV1 {
    public sealed class Identity {
        public uint ProcessId;
        public ulong CreationFileTime;
        public string ImagePath;
        public double TotalCpuMilliseconds;
        public long MonotonicTimestamp;
    }
    public sealed class Snapshot {
        public Identity Identity;
        public ulong PrivateBytes, WorkingSetBytes;
        public uint? HandleCount, GdiObjects, UserObjects;
        public string HandleCountStatus, GdiStatus, UserStatus;
    }
    public sealed class ObservedProcess : IDisposable {
        private readonly SafeProcessHandle handle;
        // No VM_READ, terminate, suspend, VM_WRITE, or remote-thread rights.
        private const uint QueryAndSynchronize = 0x00000400 | 0x00100000;
        [StructLayout(LayoutKind.Sequential)]
        private struct FileTime {
            public uint Low, High;
            public ulong Value { get { return ((ulong)High << 32) | Low; } }
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct MemoryCounters {
            public uint Size, PageFaultCount;
            public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage,
                QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage,
                PagefileUsage, PeakPagefileUsage, PrivateUsage;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern SafeProcessHandle OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint pid);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint GetProcessId(SafeProcessHandle process);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetProcessTimes(SafeProcessHandle process, out FileTime creation,
            out FileTime exit, out FileTime kernel, out FileTime user);
        [DllImport("kernel32.dll", EntryPoint = "QueryFullProcessImageNameW", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool QueryFullProcessImageName(SafeProcessHandle process, uint flags, StringBuilder path, ref uint length);
        [DllImport("psapi.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetProcessMemoryInfo(SafeProcessHandle process, ref MemoryCounters counters, uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetProcessHandleCount(SafeProcessHandle process, out uint count);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(SafeProcessHandle process, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint GetActiveProcessorCount(ushort group);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern void SetLastError(uint error);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint GetGuiResources(SafeProcessHandle process, uint flags);

        public ObservedProcess(int processId) {
            handle = OpenProcess(QueryAndSynchronize, false, checked((uint)processId));
            if (handle == null || handle.IsInvalid) {
                int error = Marshal.GetLastWin32Error();
                if (handle != null) handle.Dispose();
                throw new Win32Exception(error);
            }
        }
        public static int LogicalProcessorCount() {
            uint count = GetActiveProcessorCount(0xffff); // All processor groups.
            if (count == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
            return checked((int)count);
        }
        public bool WaitForExit(int milliseconds) {
            if (milliseconds < 0 || milliseconds > 250) throw new ArgumentOutOfRangeException("milliseconds");
            uint result = WaitForSingleObject(handle, (uint)milliseconds);
            if (result == 0) return true;
            if (result == 258) return false;
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        public bool HasExited { get { return WaitForExit(0); } }
        public Identity ReadIdentity() {
            uint pid = GetProcessId(handle);
            if (pid == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
            FileTime creation, exit, kernel, user;
            if (!GetProcessTimes(handle, out creation, out exit, out kernel, out user))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            long timestamp = Stopwatch.GetTimestamp();
            StringBuilder path = new StringBuilder(32768);
            uint length = (uint)path.Capacity;
            if (!QueryFullProcessImageName(handle, 0, path, ref length))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return new Identity { ProcessId = pid, CreationFileTime = creation.Value,
                ImagePath = path.ToString(), MonotonicTimestamp = timestamp,
                TotalCpuMilliseconds = ((double)kernel.Value + (double)user.Value) / 10000.0 };
        }
        private uint? ReadGuiCount(uint kind, out string status) {
            try {
                SetLastError(0);
                uint count = GetGuiResources(handle, kind);
                int error = Marshal.GetLastWin32Error();
                if (count > 0) { status = "Available"; return count; }
                // Zero is also the failure sentinel. Do not invent a valid zero
                // when the API supplies no error that disambiguates it.
                status = error == 0 ? "ZeroOrUnavailable" : "Win32Error:" + error.ToString(CultureInfo.InvariantCulture);
            } catch (EntryPointNotFoundException) { status = "ApiUnavailable"; }
              catch (DllNotFoundException) { status = "ApiUnavailable"; }
            return null;
        }
        public Snapshot ReadSnapshot() {
            Identity identity = ReadIdentity();
            MemoryCounters memory = new MemoryCounters();
            memory.Size = checked((uint)Marshal.SizeOf(typeof(MemoryCounters)));
            if (!GetProcessMemoryInfo(handle, ref memory, memory.Size))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            Snapshot sample = new Snapshot { Identity = identity,
                PrivateBytes = memory.PrivateUsage.ToUInt64(), WorkingSetBytes = memory.WorkingSetSize.ToUInt64() };
            uint count;
            if (GetProcessHandleCount(handle, out count)) { sample.HandleCount = count; sample.HandleCountStatus = "Available"; }
            else { sample.HandleCountStatus = "Win32Error:" + Marshal.GetLastWin32Error().ToString(CultureInfo.InvariantCulture); }
            sample.GdiObjects = ReadGuiCount(0, out sample.GdiStatus);
            sample.UserObjects = ReadGuiCount(1, out sample.UserStatus);
            return sample;
        }
        public void Dispose() { handle.Dispose(); }
    }
}
'@
    }
    $stage = 'ImagePathValidation'
    $resolved = Resolve-Path -LiteralPath $ApplicationPath
    if ($resolved.Provider.Name -ne 'FileSystem' -or (Get-Item -LiteralPath $resolved.ProviderPath).PSIsContainer) {
        throw 'ApplicationPath must name a filesystem image.'
    }
    $requestedPath = [IO.Path]::GetFullPath($resolved.ProviderPath)
    $stage = 'Attach'
    $observer = [LakeResourceObservationV1.ObservedProcess]::new($ProcessId)
    $stage = 'IdentityValidation'
    if ($observer.HasExited) { $stopReason = 'TargetExited' }
    else {
        $identity = $observer.ReadIdentity()
        if ($identity.ProcessId -ne $ProcessId -or $identity.CreationFileTime -eq 0 -or
            -not [string]::Equals([IO.Path]::GetFullPath($identity.ImagePath), $requestedPath, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'The selected PID does not match ApplicationPath.'
        }
        $stage = 'ImageHashValidation'
        if ((Get-FileHash -LiteralPath $requestedPath -Algorithm SHA256).Hash -ine $expectedSha256) {
            throw 'The image is not the supported Build 007.'
        }
        $stage = 'IdentityRevalidation'
        if ($observer.HasExited) { $stopReason = 'TargetExited' }
        else {
            $confirmed = $observer.ReadIdentity()
            if ($confirmed.ProcessId -ne $identity.ProcessId -or $confirmed.CreationFileTime -ne $identity.CreationFileTime -or
                -not [string]::Equals($confirmed.ImagePath, $identity.ImagePath, [StringComparison]::OrdinalIgnoreCase)) {
                $stopReason = 'TargetIdentityChanged'
                Write-Warning 'Identity changed; observation stopped. Lake was not changed.'
            } else {
                $validatedPath = $requestedPath
                $validatedCreation = $identity.CreationFileTime
                $creationUtc = [datetime]::FromFileTimeUtc([long]$validatedCreation).ToString('o', $invariant)
                $imageValidated = $true
                $stage = 'ProcessorCount'
                $logicalProcessorCount = [LakeResourceObservationV1.ObservedProcess]::LogicalProcessorCount()
                $metrics = Get-LakeResourceSummary -LogicalProcessorCount $logicalProcessorCount
                $observationStartedUtc = [datetime]::UtcNow.ToString('o', $invariant)
                $originTicks = [Diagnostics.Stopwatch]::GetTimestamp()
                $tickToMs = 1000.0 / [Diagnostics.Stopwatch]::Frequency
                $durationMs = $DurationSeconds * 1000.0
                $intervalMs = $IntervalSeconds * 1000.0
                $nextSampleMs = 0.0
                $previousCpu = $null; $previousElapsed = $null
                while ($true) {
                    $stage = 'ProcessMetadata'
                    if ($observer.HasExited) { $stopReason = 'TargetExited'; break }
                    if ($metrics.SampleCount -ge $MaximumSamples) { $stopReason = 'MaximumSamplesReached'; break }
                    $nowMs = ([Diagnostics.Stopwatch]::GetTimestamp() - $originTicks) * $tickToMs
                    if ($nowMs -ge $durationMs) { $stopReason = 'DurationElapsed'; break }
                    if ($nowMs -lt $nextSampleMs) {
                        $waitMs = [int][math]::Ceiling([math]::Min(250.0, [math]::Min($nextSampleMs - $nowMs, $durationMs - $nowMs)))
                        if ($observer.WaitForExit($waitMs)) { $stopReason = 'TargetExited'; break }
                        continue
                    }
                    $native = $observer.ReadSnapshot()
                    if ($observer.HasExited) { $stopReason = 'TargetExited'; break }
                    $elapsedMs = ($native.Identity.MonotonicTimestamp - $originTicks) * $tickToMs
                    $decision = Get-LakeObservationStopReason -HasExited $false -ExpectedPid $ProcessId -ActualPid $native.Identity.ProcessId `
                        -ExpectedCreation $validatedCreation -ActualCreation $native.Identity.CreationFileTime `
                        -ExpectedPath $validatedPath -ActualPath ([IO.Path]::GetFullPath($native.Identity.ImagePath)) `
                        -SampleCount $metrics.SampleCount -MaximumSamples $MaximumSamples `
                        -ElapsedMilliseconds $elapsedMs -DurationMilliseconds $durationMs
                    if ($null -ne $decision) { $stopReason = $decision; break }
                    $stage = 'Metrics'
                    $cpu = [pscustomobject]@{ IsValid = $false; InvalidReason = 'NoPreviousSample'
                        CpuDeltaMilliseconds = $null; WallDeltaMilliseconds = $null
                        CpuPercentOneCore = $null; CpuPercentMachine = $null }
                    if ($metrics.SampleCount -gt 0) {
                        $cpu = Get-LakeCpuMetrics -PreviousCpuMilliseconds $previousCpu `
                            -CurrentCpuMilliseconds $native.Identity.TotalCpuMilliseconds `
                            -ElapsedMilliseconds ($elapsedMs - $previousElapsed) -LogicalProcessorCount $logicalProcessorCount
                    }
                    $sample = [pscustomobject][ordered]@{
                        SampleIndex = [long]$metrics.SampleCount
                        TimestampUtc = [datetime]::UtcNow.ToString('o', $invariant)
                        ElapsedMilliseconds = $elapsedMs
                        TotalCpuMilliseconds = $native.Identity.TotalCpuMilliseconds
                        WallDeltaMilliseconds = $cpu.WallDeltaMilliseconds
                        CpuDeltaMilliseconds = $cpu.CpuDeltaMilliseconds
                        CpuPercentOneCore = $cpu.CpuPercentOneCore
                        CpuPercentMachine = $cpu.CpuPercentMachine
                        CpuValid = $cpu.IsValid; CpuInvalidReason = $cpu.InvalidReason
                        PrivateBytes = $native.PrivateBytes; WorkingSetBytes = $native.WorkingSetBytes
                        HandleCount = $native.HandleCount; HandleCountStatus = $native.HandleCountStatus
                        GdiObjects = $native.GdiObjects; GdiStatus = $native.GdiStatus
                        UserObjects = $native.UserObjects; UserStatus = $native.UserStatus
                    }
                    $updatedMetrics = Get-LakeResourceSummary -Samples @($sample) `
                        -LogicalProcessorCount $logicalProcessorCount -PreviousSummary $metrics
                    $values = [System.Collections.Generic.List[object]]::new()
                    foreach ($column in $columns) { $values.Add($sample.$column) }
                    $stage = 'CsvOutput'
                    $csvWriter.WriteLine((Get-LakeCsvLine ($values.ToArray()))) # AutoFlush after each row.
                    $metrics = $updatedMetrics
                    $previousCpu = $sample.TotalCpuMilliseconds; $previousElapsed = $elapsedMs
                    if ($MaximumPrivateMiB -gt 0 -and ([double]$native.PrivateBytes / 1048576.0) -ge $MaximumPrivateMiB) {
                        $stopReason = 'MaximumPrivateMiBReached'
                        Write-Warning 'Private-memory observation threshold reached. Only observation stopped; the monitor did not stop or change Lake.'
                        break
                    }
                    # Skip missed slots rather than fabricate samples or burst-query
                    # after an unusually slow metadata/output operation.
                    $nextSampleMs += $intervalMs
                    $afterMs = ([Diagnostics.Stopwatch]::GetTimestamp() - $originTicks) * $tickToMs
                    if ($nextSampleMs -lt $afterMs) {
                        $skipped = [long]([math]::Floor(($afterMs - $nextSampleMs) / $intervalMs) + 1)
                        $skippedSampleSlots += $skipped
                        $nextSampleMs += $skipped * $intervalMs
                    }
                }
            }
        }
    }
} catch {
    $failure = $_.Exception
    $errorStage = $stage
    $details = Get-LakeFailureDetails $failure
    if ($details.Type -eq 'System.Management.Automation.PipelineStoppedException') { $stopReason = 'Interrupted' }
    elseif ($stage -in @('ProcessMetadata', 'IdentityValidation', 'IdentityRevalidation') -and $null -ne $observer) {
        $exited = $false
        try { $exited = $observer.HasExited } catch { }
        if ($exited) { $stopReason = 'TargetExited'; $failure = $null; $errorStage = $null }
        elseif ($stage -eq 'IdentityValidation') { $stopReason = 'ValidationFailed' }
        else { $stopReason = 'ObservationError' }
    } elseif ($stage -in @('OutputInitialization', 'CsvOutput')) { $stopReason = 'OutputError' }
    elseif ($stage -in @('ImagePathValidation', 'ImageHashValidation', 'Attach', 'PlatformValidation')) { $stopReason = 'ValidationFailed' }
    else { $stopReason = 'ObservationError' }
} finally {
    $endedUtc = [datetime]::UtcNow.ToString('o', $invariant)
    if ($null -ne $originTicks) {
        $observationElapsedMs = ([Diagnostics.Stopwatch]::GetTimestamp() - $originTicks) * (1000.0 / [Diagnostics.Stopwatch]::Frequency)
    }
    try { if ($null -ne $observer) { $observer.Dispose() } }
    catch {
        if ($null -eq $failure) { $failure = $_.Exception; $errorStage = 'HandleFinalization' }
        $stopReason = 'ObservationError'
    }
    try {
        if ($null -ne $csvWriter) { $csvWriter.Dispose() }
        elseif ($null -ne $csvStream) { $csvStream.Dispose() }
    } catch {
        if ($null -eq $failure) { $failure = $_.Exception; $errorStage = 'CsvFinalization' }
        $stopReason = 'OutputError'
    }
    $details = Get-LakeFailureDetails $failure
    if ($null -ne $jsonStream) {
        try {
            $document = [ordered]@{
                SchemaVersion = 1; RunId = $runId; Label = $Label; ProcessId = $ProcessId
                ExpectedImageSha256 = $expectedSha256; ImageValidationPassed = $imageValidated
                ValidatedApplicationPath = $validatedPath
                ProcessCreationUtc = $creationUtc
                ProcessCreationFileTimeUtc = $(if ($null -ne $validatedCreation) { $validatedCreation.ToString($invariant) } else { $null })
                RunStartedUtc = $runStartedUtc.ToString('o', $invariant)
                ObservationStartedUtc = $observationStartedUtc; EndedUtc = $endedUtc
                RequestedDurationSeconds = $DurationSeconds; RequestedIntervalSeconds = $IntervalSeconds
                MaximumPrivateMiB = $MaximumPrivateMiB; LogicalProcessorCount = $logicalProcessorCount
                MaximumSamples = $MaximumSamples
                ObservationElapsedMilliseconds = $observationElapsedMs; SkippedSampleSlots = $skippedSampleSlots
                StopReason = $stopReason; ErrorStage = $errorStage
                ErrorType = $details.Type; Win32ErrorCode = $details.Win32ErrorCode
                CsvFile = [IO.Path]::GetFileName($csvPath); Metrics = $metrics
                Notes = @(
                    'Finite observations do not prove a leak or its cause. Do not linearly extrapolate to hours.',
                    'Private bytes are private committed memory; working set is resident memory and can include shared pages.',
                    'CPU uses process kernel+user milliseconds and Stopwatch wall time. One-core percent is divided by all-group logical CPU count for machine percent; neither is capped.',
                    'The first sample has no CPU interval. Invalid intervals and unavailable/ambiguous GUI counts are null, not zero.',
                    'Counters are not an atomic snapshot. Actual monotonic sample times, not requested cadence, determine CPU intervals.',
                    'The monitor and output IO consume resources and can perturb workload. Interop initialization precedes the observation clock.',
                    'The EXE hash checks a disk file, not loaded code. This does not establish equivalence of DLLs, flags, presets, or application activity.',
                    'Only metadata was queried. Lake was not started, stopped, suspended, reconfigured, or sent input.',
                    'CSV is flushed per row; abrupt host termination or output errors can prevent complete final JSON.'
                )
            }
            $jsonWriter = [IO.StreamWriter]::new($jsonStream, [Text.UTF8Encoding]::new($false))
            $jsonWriter.WriteLine((ConvertTo-Json -InputObject $document -Depth 10))
            $jsonWriter.Flush()
            $summaryWritten = $true
        } catch {
            if ($null -eq $failure) { $failure = $_.Exception; $errorStage = 'SummaryOutput' }
            $stopReason = 'OutputError'
        } finally {
            try {
                if ($null -ne $jsonWriter) { $jsonWriter.Dispose() }
                else { $jsonStream.Dispose() }
            } catch {
                if ($null -eq $failure) { $failure = $_.Exception; $errorStage = 'SummaryFinalization' }
                $stopReason = 'OutputError'
                $summaryWritten = $false
            }
        }
    }
}

Write-Host "Observation stopped: $stopReason. Lake was not changed."
if ($null -ne $csvPath) { Write-Host "CSV: $csvPath" }
if ($summaryWritten) { Write-Host "Summary: $jsonPath" }
if ($null -ne $failure) {
    $details = Get-LakeFailureDetails $failure
    throw "Resource observation failed at '$errorStage' ($($details.Type)); StopReason=$stopReason. Final JSON written: $summaryWritten."
}
[pscustomobject]@{ StopReason = $stopReason; CsvPath = $csvPath; SummaryPath = $jsonPath }
