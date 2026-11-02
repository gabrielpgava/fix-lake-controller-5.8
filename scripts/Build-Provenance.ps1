#Requires -Version 5.1
# Dot-source library. No compiler, executable, GitHub command or network operation
# runs on import. The build scripts own execution; these functions attest it.

function Get-LakeRuntimeNames {
    @('LakeWindowedLauncher.exe','LakeWindowedFix.dll','LakeWindowedFix.ini','LICENSE',
        'Measure-LakeResources.ps1','LakeResourceMetrics.psm1')
}

function Get-LakeRequiredTests([string]$Component) {
    if ($Component -eq 'Runtime') {
        @('scale_geometry','ui_quality_policy','scaled_presenter_objects','profiling_disabled','profiling_enabled',
            'resource_metrics_policy','isolated_build_options','launcher_patch_icon','windows_safe_no_rebuild',
            'windows_experimental_resize','windows_exit_observer','windows_scaled_window',
            'startup_policy','session_configuration','component_contract','diagnostics_queue','monitor_lifecycle_policy','window_state_snapshots','own_link_symbols')
    } elseif ($Component -eq 'Setup') { @('installer_sandbox','installer_payload_integrity','installer_localization') }
    else { throw 'Unknown provenance component.' }
}

function Get-LakeReleaseNotes {
    @('PATCH ONLY. Lake Controller, Contour and other proprietary software are NOT included; a legitimate original installation and a license where required are necessary.',
        'The patched desktop shortcut uses a red PATCHED badge; the original Lake shortcut and icon remain untouched for legacy fallback.',
        'No real Lake OFFLINE validation for this candidate; no field/hardware approval or proven FPS/leak fix.',
        'Unsigned per-user installer; review Windows/EDR prompts according to local policy.',
        'Independent unofficial project, not endorsed by the manufacturer. MIT covers only the project files, not third-party software or trademarks.')
}

function Assert-LakeFields($Value,[string[]]$Names,[string]$Label) {
    if ($null -eq $Value) { throw "Missing $Label." }
    $actual = @($Value.PSObject.Properties.Name)
    if ($actual.Count -ne $Names.Count -or @($actual | Where-Object { $_ -cnotin $Names }).Count) {
        throw "Unexpected $Label fields. Private/unrecognized metadata is not distributable."
    }
}

function Assert-LakeSafePath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $cursor = $full
    while ($cursor) {
        # Get-Item also sees dangling links; Test-Path alone may hide them.
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -ne $item) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point refused: $cursor" }
        }
        $parent = Split-Path $cursor -Parent
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $full
}

function Get-LakeSafeFile([string]$Path,[long]$MaximumBytes = 67108864) {
    $full = Assert-LakeSafePath $Path
    $file = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    if ($file.PSIsContainer -or $file.Length -lt 1 -or $file.Length -gt $MaximumBytes) { throw "Unsafe/empty file: $full" }
    return $file
}

function Get-LakeTextHash([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-LakeFileRecord([string]$Path,[string]$Name,[long]$MaximumBytes = 67108864) {
    $file = Get-LakeSafeFile $Path $MaximumBytes
    [pscustomobject][ordered]@{Name=$Name;Sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant();Bytes=$file.Length}
}

function Get-LakeRecordsHash($Records) {
    Get-LakeTextHash ((@($Records | ForEach-Object { "$($_.Name)|$($_.Sha256)|$($_.Bytes)" })) -join "`n")
}

function Write-LakeJson([string]$Path,$Value) {
    $full = Assert-LakeSafePath $Path
    [IO.Directory]::CreateDirectory((Split-Path $full -Parent)) | Out-Null
    # Keep temporary names short for Windows PowerShell 5.1/MAX_PATH.
    $temporary = Join-Path (Split-Path $full -Parent) ('.'+[guid]::NewGuid().ToString('N')+'.tmp')
    [IO.File]::WriteAllText($temporary,($Value | ConvertTo-Json -Depth 24)+"`n",[Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $full) {
        $null = Get-LakeSafeFile $full 4194304
        [IO.File]::Replace($temporary,$full,[NullString]::Value)
    } else { [IO.File]::Move($temporary,$full) }
}

function Read-LakeJson([string]$Path) {
    $file = Get-LakeSafeFile $Path 4194304
    return ([IO.File]::ReadAllText($file.FullName) | ConvertFrom-Json -ErrorAction Stop)
}

function Invoke-LakeProvenanceGit([string]$Root,[string[]]$Arguments) {
    $lines = @(& git -C $Root @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Cannot capture source provenance: git $($Arguments[0])." }
    return $lines
}

function Get-LakeSourceIdentity([string]$Root) {
    $head = (Invoke-LakeProvenanceGit $Root @('rev-parse','HEAD')) -join ''
    if ($head -cnotmatch '^[a-f0-9]{40}$') { throw 'Invalid source commit.' }
    $status = @(Invoke-LakeProvenanceGit $Root @('status','--porcelain','--untracked-files=normal'))
    # Hash source contents, not only porcelain flags: a file may change twice
    # while remaining " M". Ignored build/dist/private outputs are not inputs.
    $paths = @(Invoke-LakeProvenanceGit $Root @('ls-files','--cached','--others','--exclude-standard')) | Sort-Object -Unique
    if (!$paths.Count) { throw 'Cannot fingerprint an empty source inventory.' }
    $records = foreach ($relative in $paths) {
        if ($relative -match '[\x00-\x1f"]' -or [IO.Path]::IsPathRooted($relative) -or
            @($relative -split '[/\\]' | Where-Object { $_ -eq '..' }).Count) { throw 'Unsafe source inventory path.' }
        $path = Assert-LakeSafePath (Join-Path $Root $relative)
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item.PSIsContainer) { throw 'Submodules/directories are not supported source inputs.' }
            "$relative|$((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant())"
        } else { "$relative|missing" }
    }
    [pscustomobject]@{Commit=$head;Dirty=($status.Count -ne 0);Fingerprint=(Get-LakeTextHash (($head+"`n"+($status -join "`n")+"`n"+($records -join "`n"))))}
}

function Get-LakeArtifactRecords([string]$Directory,[string[]]$Names) {
    foreach ($name in $Names) { Get-LakeFileRecord (Join-Path $Directory $name) $name }
}

function Assert-LakeRecords($Records,[string[]]$Names,[string]$Label) {
    if (@($Records).Count -ne $Names.Count) { throw "Unexpected $Label file count." }
    for ($i=0; $i -lt $Names.Count; $i++) {
        $record = @($Records)[$i]
        Assert-LakeFields $record @('Name','Sha256','Bytes') "$Label file"
        if ($record.Name -cne $Names[$i] -or $record.Sha256 -cnotmatch '^[a-f0-9]{64}$' -or
            $record.Bytes -isnot [ValueType] -or $record.Bytes -lt 1 -or $record.Bytes -gt 67108864 -or
            [long]$record.Bytes -ne $record.Bytes) { throw "Invalid $Label file record." }
    }
}

function Assert-LakeSameRecords($Left,$Right,[string]$Label) {
    if ((Get-LakeRecordsHash $Left) -cne (Get-LakeRecordsHash $Right)) { throw "Mismatched $Label hashes. Components were changed or mixed." }
}

function Assert-LakeX86Pe([string]$Path) {
    $file = Get-LakeSafeFile $Path
    $stream = [IO.File]::OpenRead($file.FullName)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 70 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Expected a Windows x86 PE binary.' }
        $stream.Position = 60
        $pe = $reader.ReadInt32()
        if ($pe -lt 64 -or $pe -gt $stream.Length-6) { throw 'Invalid PE header offset.' }
        $stream.Position = $pe
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c) { throw 'Expected a Windows x86 PE binary.' }
    } finally { $reader.Dispose(); $stream.Dispose() }
}

function Assert-LakeBuildDocument($Document,[switch]$Public,[switch]$RequireTests,[switch]$AllowUnsignedDevelopment) {
    $fields = @('Schema','Project','State','Component','BuildId','Version','SourceCommit','SourceDirty','SourceFingerprint',
        'AbiVersion','ConfigSchema','Toolchain','Architecture','Configuration','Profiling','DistributionProfile','ArgumentsSha256',
        'StartedUtc','CompletedUtc','Files','Tests','RuntimeManifestSha256','PayloadManifestSha256','Payload')
    if ($Public) { $fields += 'ManifestSha256' } else { $fields += 'Arguments' }
    Assert-LakeFields $Document $fields 'build provenance'
    if ($Document.Schema -ne 1 -or $Document.Project -cne 'lake-controller-windowed-fix' -or $Document.State -cne 'Complete' -or
        $Document.Component -cnotin @('Runtime','Setup') -or $Document.BuildId -cnotmatch '^[a-f0-9]{32}$' -or
        $Document.Version -cnotmatch '^\d+\.\d+\.\d+$' -or $Document.SourceCommit -cnotmatch '^[a-f0-9]{40}$' -or
        $Document.SourceDirty -isnot [bool] -or $Document.SourceFingerprint -cnotmatch '^[a-f0-9]{64}$' -or
        $Document.AbiVersion -isnot [ValueType] -or $Document.AbiVersion -lt 1 -or [int]$Document.AbiVersion -ne $Document.AbiVersion -or
        $Document.ConfigSchema -isnot [ValueType] -or $Document.ConfigSchema -lt 1 -or [int]$Document.ConfigSchema -ne $Document.ConfigSchema -or
        $Document.Architecture -cne 'x86' -or $Document.Configuration -cnotin @('Debug','Release') -or
        $Document.Profiling -isnot [bool] -or $Document.ArgumentsSha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'Invalid/incomplete build provenance. Rebuild; do not stamp an old dist.' }
    foreach ($time in @($Document.StartedUtc,$Document.CompletedUtc)) {
        if ($time -isnot [string] -or $time -cnotmatch '^\d{4}-\d{2}-\d{2}T[0-9:.]+Z$') { throw 'Invalid build timestamp.' }
        $null = [DateTime]::Parse($time,[Globalization.CultureInfo]::InvariantCulture)
    }
    if ([DateTime]::Parse($Document.CompletedUtc) -lt [DateTime]::Parse($Document.StartedUtc)) { throw 'Build timestamps are reversed.' }
    Assert-LakeFields $Document.Toolchain @('Name','Version','CompilerSha256') 'toolchain'
    if ($Document.Toolchain.Name -cnotin @('Zig','MSVC','MinGW') -or
        $Document.Toolchain.Version -cnotmatch '^[0-9][A-Za-z0-9.+ -]{0,95}$' -or
        $Document.Toolchain.CompilerSha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'Invalid toolchain identity.' }
    if ($Document.DistributionProfile -cne 'DevelopmentUnsigned') { throw 'Signed profile is not implemented; no signature is claimed.' }
    if (!$AllowUnsignedDevelopment) { throw 'Unsigned development artifacts require explicit -AllowUnsignedDevelopment.' }
    if ($Public) {
        if ($Document.ManifestSha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'Invalid provenance manifest hash.' }
    } else {
        if (@($Document.Arguments | Where-Object { $_ -isnot [string] }).Count -or
            (Get-LakeTextHash (ConvertTo-Json -InputObject @($Document.Arguments) -Compress)) -cne $Document.ArgumentsSha256) { throw 'Build argument capture is inconsistent.' }
    }
    $names = @(Get-LakeRuntimeNames)
    if ($Document.Component -eq 'Setup') {
        if (@($Document.Files).Count -ne 1) { throw 'Setup provenance requires exactly one executable.' }
        $name = @($Document.Files)[0].Name
        $short = ($Document.Version -split '\.')[0..1] -join '.'
        if ($name -cne "LakeWindowedFix-Setup-v$($Document.Version).exe" -and $name -cne "LakeWindowedFix-Setup-v$short.exe") { throw 'Unexpected setup executable name.' }
        $names = @($name)
        if ($Document.RuntimeManifestSha256 -cnotmatch '^[a-f0-9]{64}$' -or $Document.PayloadManifestSha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'Missing setup/runtime payload links.' }
        Assert-LakePayloadDocument $Document.Payload
        if ((Get-LakeTextHash (($Document.Payload | ConvertTo-Json -Depth 24)+"`n")) -cne $Document.PayloadManifestSha256) { throw 'Payload manifest hash is inconsistent.' }
    } elseif ($null -ne $Document.RuntimeManifestSha256 -or $null -ne $Document.PayloadManifestSha256 -or $null -ne $Document.Payload) {
        throw 'Runtime provenance may not carry setup metadata.'
    }
    Assert-LakeRecords $Document.Files $names 'provenance'
    Assert-LakeFields $Document.Tests @('BuildId','FilesSha256','Results') 'test evidence'
    if ($Document.Tests.BuildId -cne $Document.BuildId) { throw 'Tests belong to another build.' }
    $results = @($Document.Tests.Results)
    if ($results.Count -or $RequireTests) {
        if ($Document.Tests.FilesSha256 -cne (Get-LakeRecordsHash $Document.Files)) { throw 'Tests do not cover these exact artifacts.' }
    } elseif ($null -ne $Document.Tests.FilesSha256) { throw 'Unexpected untested artifact digest.' }
    $required = @(Get-LakeRequiredTests $Document.Component)
    $seen = @()
    foreach ($test in $results) {
        Assert-LakeFields $test @('Name','ExitCode','EvidenceSha256') 'test result'
        if ($test.Name -cnotin $required -or $test.Name -cin $seen -or $test.ExitCode -isnot [ValueType] -or
            [int]$test.ExitCode -ne $test.ExitCode -or $test.EvidenceSha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'Invalid/incompatible test evidence.' }
        if ($RequireTests -and $test.ExitCode -ne 0) { throw "Failed test: $($test.Name)." }
        $seen += $test.Name
    }
    if ($RequireTests -and ($seen.Count -ne $required.Count -or @($required | Where-Object { $_ -cnotin $seen }).Count)) { throw 'Required test evidence is missing. SkipTests artifacts cannot be packaged.' }
}

function Assert-LakePayloadDocument($Payload) {
    Assert-LakeFields $Payload @('Schema','Project','Kind','RuntimeBuildId','RuntimeManifestSha256','Version','SourceCommit',
        'SourceDirty','SourceFingerprint','AbiVersion','ConfigSchema','Architecture','Configuration','Profiling','DistributionProfile','Files','HeaderSha256') 'payload manifest'
    if ($Payload.Schema -ne 1 -or $Payload.Project -cne 'lake-controller-windowed-fix' -or $Payload.Kind -cne 'InstallerPayload' -or
        $Payload.RuntimeBuildId -cnotmatch '^[a-f0-9]{32}$' -or $Payload.RuntimeManifestSha256 -cnotmatch '^[a-f0-9]{64}$' -or
        $Payload.HeaderSha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'Invalid installer payload manifest.' }
    Assert-LakeRecords $Payload.Files @(Get-LakeRuntimeNames) 'payload'
}

function Assert-LakePayloadMatchesRuntime($Payload,$Runtime,[string]$RuntimeHash) {
    Assert-LakePayloadDocument $Payload
    if ($Payload.RuntimeBuildId -cne $Runtime.BuildId -or $Payload.RuntimeManifestSha256 -cne $RuntimeHash) { throw 'Payload belongs to another runtime build.' }
    foreach ($field in @('Version','SourceCommit','SourceDirty','SourceFingerprint','AbiVersion','ConfigSchema','Architecture','Configuration','Profiling','DistributionProfile')) {
        if ($Payload.$field -cne $Runtime.$field) { throw "Payload/runtime mismatch: $field." }
    }
    Assert-LakeSameRecords $Payload.Files $Runtime.Files 'payload/runtime'
}

function Get-LakeProvenanceProductMetadata([string]$Root) {
    $file = Get-LakeSafeFile (Join-Path $Root 'src\product_version.h') 65536
    $text = [IO.File]::ReadAllText($file.FullName)
    $version = [regex]::Match($text,'(?m)^\s*#define\s+LAKE_FIX_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"\s*$').Groups[1].Value
    $abi = [regex]::Match($text,'(?m)^\s*#define\s+LAKE_FIX_ABI_VERSION\s+(\d+)\s*$').Groups[1].Value
    $schema = [regex]::Match($text,'(?m)^\s*#define\s+LAKE_FIX_CONFIG_SCHEMA\s+(\d+)\s*$').Groups[1].Value
    if (!$version -or !$abi -or !$schema -or [int]$abi -lt 1 -or [int]$schema -lt 1) { throw 'Missing product version/ABI/config schema metadata.' }
    [pscustomobject]@{Version=$version;AbiVersion=[int]$abi;ConfigSchema=[int]$schema}
}

function Start-LakeBuildProvenance {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$ManifestPath,
        [Parameter(Mandatory)][ValidateSet('Runtime','Setup')][string]$Component,
        [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
        [Parameter(Mandatory)][ValidateSet('Zig','MSVC','MinGW')][string]$Toolchain,
        [Parameter(Mandatory)][ValidatePattern('^[0-9][A-Za-z0-9.+ -]{0,95}$')][string]$ToolchainVersion,
        [Parameter(Mandatory)][string]$CompilerPath,
        [ValidateSet('x86')][string]$Architecture = 'x86',
        [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
        [Parameter(Mandatory)][ValidateSet('DevelopmentUnsigned','Signed')][string]$DistributionProfile,
        [AllowEmptyCollection()][string[]]$BuildArguments = @(),
        [switch]$EnableProfiling,
        [string]$RuntimeManifestPath,
        [string]$InstallerFileName,
        [string]$GeneratedHeader,
        [string]$PayloadManifestPath,
        [string]$PayloadHeaderPath
    )
    if ($DistributionProfile -ne 'DevelopmentUnsigned') { throw 'Signed profile is pending. Use real signing outside this API; do not claim a signature.' }
    $root = Assert-LakeSafePath $RepositoryRoot
    $path = Assert-LakeSafePath $ManifestPath
    $metadata = Get-LakeProvenanceProductMetadata $root
    if ($metadata.Version -cne $Version) { throw 'Build version does not match its source project.' }
    $source = Get-LakeSourceIdentity $root
    # A complete Zig driver is larger than the runtime/payload size budget.
    # Keep the artifact whitelist limit, but hash compilers up to 1 GiB by stream.
    $compiler = Get-LakeFileRecord $CompilerPath 'compiler' 1073741824
    $inputs = @()
    $runtimeDocument = $null
    $runtimeHash = $null
    $payload = $null
    $payloadHash = $null
    if ($Component -eq 'Runtime') {
        $inputs = @(Get-LakeFileRecord (Join-Path $root 'config\LakeWindowedFix.ini') 'LakeWindowedFix.ini'
            Get-LakeFileRecord (Join-Path $root 'LICENSE') 'LICENSE'
            Get-LakeFileRecord (Join-Path $root 'scripts\Measure-LakeResources.ps1') 'Measure-LakeResources.ps1'
            Get-LakeFileRecord (Join-Path $root 'scripts\LakeResourceMetrics.psm1') 'LakeResourceMetrics.psm1')
        $names = @(Get-LakeRuntimeNames)
    } else {
        if (!$RuntimeManifestPath) { throw 'Setup begin requires -RuntimeManifestPath.' }
        $runtimeDocument = Read-LakeJson $RuntimeManifestPath
        Assert-LakeBuildDocument $runtimeDocument -RequireTests -AllowUnsignedDevelopment
        if ($runtimeDocument.Component -cne 'Runtime' -or $runtimeDocument.Version -cne $Version -or
            $runtimeDocument.SourceCommit -cne $source.Commit -or $runtimeDocument.SourceDirty -cne $source.Dirty -or
            $runtimeDocument.SourceFingerprint -cne $source.Fingerprint) { throw 'Setup must be built from the exact runtime source snapshot.' }
        $runtimeHash = (Get-LakeFileRecord $RuntimeManifestPath 'manifest').Sha256
        if (!$PayloadManifestPath) { throw 'Setup begin requires -PayloadManifestPath, frozen before compilation.' }
        if (!$PayloadHeaderPath) { $PayloadHeaderPath = $PayloadManifestPath -replace '\.manifest\.json$','' }
        $payload = Read-LakeJson $PayloadManifestPath
        Assert-LakePayloadMatchesRuntime $payload $runtimeDocument $runtimeHash
        if ((Get-LakeFileRecord $PayloadHeaderPath 'header').Sha256 -cne $payload.HeaderSha256) { throw 'Payload header does not match its manifest.' }
        $payloadHash = (Get-LakeFileRecord $PayloadManifestPath 'payload-manifest').Sha256
        if (!$InstallerFileName) { $InstallerFileName = "LakeWindowedFix-Setup-v$Version.exe" }
        $short = ($Version -split '\.')[0..1] -join '.'
        if ($InstallerFileName -cne "LakeWindowedFix-Setup-v$Version.exe" -and $InstallerFileName -cne "LakeWindowedFix-Setup-v$short.exe") { throw 'Unsafe installer filename.' }
        $names = @($InstallerFileName)
    }
    if (Test-Path -LiteralPath $path) {
        $old = Read-LakeJson $path
        if ($old.Project -cne 'lake-controller-windowed-fix' -or $old.Schema -ne 1 -or $old.Component -cne $Component -or
            $old.State -cnotin @('Building','Complete')) { throw 'Foreign provenance file preserved.' }
        if ($old.State -ceq 'Complete') { Assert-LakeBuildDocument $old -AllowUnsignedDevelopment }
        else {
            Assert-LakeFields $old @('Schema','Project','State','Component','BuildId','Version','SourceCommit','SourceDirty','SourceFingerprint',
                'AbiVersion','ConfigSchema','Toolchain','Architecture','Configuration','Profiling','DistributionProfile','ArgumentsSha256',
                'Arguments','StartedUtc','CompletedUtc','Files','Tests','RuntimeManifestSha256','PayloadManifestSha256','Payload') 'pending build provenance'
            if ($old.BuildId -cnotmatch '^[a-f0-9]{32}$') { throw 'Foreign pending provenance preserved.' }
        }
    }
    $document = [pscustomobject][ordered]@{
        Schema=1;Project='lake-controller-windowed-fix';State='Building';Component=$Component;BuildId=[guid]::NewGuid().ToString('N')
        Version=$Version;SourceCommit=$source.Commit;SourceDirty=$source.Dirty;SourceFingerprint=$source.Fingerprint
        Toolchain=[pscustomobject]@{Name=$Toolchain;Version=$ToolchainVersion;CompilerSha256=$compiler.Sha256}
        Architecture=$Architecture;Configuration=$Configuration;Profiling=[bool]$EnableProfiling;DistributionProfile=$DistributionProfile
        ArgumentsSha256=(Get-LakeTextHash (ConvertTo-Json -InputObject @($BuildArguments) -Compress));Arguments=@($BuildArguments)
        StartedUtc=[DateTime]::UtcNow.ToString('o');CompletedUtc=$null;Files=@();Tests=$null
        RuntimeManifestSha256=$runtimeHash;PayloadManifestSha256=$payloadHash;Payload=$payload
        AbiVersion=$metadata.AbiVersion;ConfigSchema=$metadata.ConfigSchema
    }
    # Invalidate the previous completion before a compiler can touch outputs.
    Write-LakeJson $path $document
    $stampHash = $null
    if ($GeneratedHeader) {
        $header = Assert-LakeSafePath $GeneratedHeader
        $marker = '// Generated by Build-Provenance.ps1; do not edit.'
        if (Test-Path -LiteralPath $header) {
            $null = Get-LakeSafeFile $header 65536
            if (![IO.File]::ReadAllText($header).StartsWith($marker)) { throw 'Foreign build ID header preserved.' }
        }
        [IO.Directory]::CreateDirectory((Split-Path $header -Parent)) | Out-Null
        [IO.File]::WriteAllText($header,$marker+"`n#pragma once`n#define LAKE_FIX_BUILD_ID `"$($document.BuildId)`"`n",[Text.Encoding]::ASCII)
        $GeneratedHeader = $header
        $stampHash = (Get-LakeFileRecord $header 'build-stamp').Sha256
    }
    [pscustomobject]@{RepositoryRoot=$root;ManifestPath=$path;Document=$document;Names=$names;Inputs=$inputs
        RuntimeDocument=$runtimeDocument;RuntimeManifestPath=$RuntimeManifestPath;CompilerPath=$CompilerPath
        GeneratedHeader=$GeneratedHeader;GeneratedHeaderSha256=$stampHash
        PayloadManifestPath=$PayloadManifestPath;PayloadHeaderPath=$PayloadHeaderPath
        ArtifactDirectory=$null;TestedFiles=@();TestStartedUtc=$null
        Results=[Collections.Generic.List[object]]::new();Evidence=[Collections.Generic.List[object]]::new()}
}

function Assert-LakeBuildContext($Context) {
    $pending = Read-LakeJson $Context.ManifestPath
    if ($Context.Document.State -cne 'Building' -or $pending.State -cne 'Building' -or $pending.BuildId -cne $Context.Document.BuildId) { throw 'Build context is stale or already completed.' }
}

function Add-LakeBuildCommand {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Context,[Parameter(Mandatory)][string]$Program,[string[]]$Arguments=@())
    Assert-LakeBuildContext $Context
    if ($Context.ArtifactDirectory) { throw 'Compilation commands cannot be appended after freezing the test snapshot.' }
    # Windows PowerShell 5.1 silently omits empty native argv entries. Refuse
    # them rather than record a command line different from the actual call.
    if (@($Arguments | Where-Object { [string]::IsNullOrEmpty($_) }).Count) { throw 'Empty native build argument cannot be attested on PowerShell 5.1.' }
    $Context.Document.Arguments += (@(@($Program)+$Arguments | ForEach-Object { ConvertTo-LakeProcessArgument $_ }) -join ' ')
    $Context.Document.ArgumentsSha256 = Get-LakeTextHash (ConvertTo-Json -InputObject @($Context.Document.Arguments) -Compress)
    Write-LakeJson $Context.ManifestPath $Context.Document
}

function Start-LakeBuildTests {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Context,[Parameter(Mandatory)][string]$ArtifactDirectory)
    Assert-LakeBuildContext $Context
    if ($Context.ArtifactDirectory) { throw 'Test snapshot is already frozen; begin a new build instead of reusing evidence.' }
    $Context.ArtifactDirectory = Assert-LakeSafePath $ArtifactDirectory
    $Context.TestedFiles = @(Get-LakeArtifactRecords $Context.ArtifactDirectory $Context.Names)
    $Context.TestStartedUtc = [DateTime]::UtcNow
}

function Add-LakeBuildTestResult {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Context,[Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][int]$ExitCode,[Parameter(Mandatory)][string]$EvidencePath)
    Assert-LakeBuildContext $Context
    if (!$Context.ArtifactDirectory) { throw 'Call Start-LakeBuildTests before executing tests.' }
    if ($Name -cnotin @(Get-LakeRequiredTests $Context.Document.Component) -or $Name -cin @($Context.Results | ForEach-Object { $_.Name })) { throw 'Unknown/duplicate test name.' }
    Assert-LakeSameRecords $Context.TestedFiles @(Get-LakeArtifactRecords $Context.ArtifactDirectory $Context.Names) 'test/artifact'
    if ((Get-LakeSafeFile $EvidencePath).LastWriteTimeUtc -lt $Context.TestStartedUtc) { throw 'Test evidence predates this build/test snapshot.' }
    $record = Get-LakeFileRecord $EvidencePath 'evidence'
    $Context.Results.Add([pscustomobject]@{Name=$Name;ExitCode=$ExitCode;EvidenceSha256=$record.Sha256})
    $Context.Evidence.Add([pscustomobject]@{Path=[IO.Path]::GetFullPath($EvidencePath);Sha256=$record.Sha256})
    if ($ExitCode -ne 0) { throw "Test $Name failed (exit $ExitCode). Provenance remains incomplete." }
}

function Invoke-LakeBuildTest {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Context,[Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Program,[string[]]$Arguments = @())
    Assert-LakeBuildContext $Context
    if (!$Context.ArtifactDirectory) { throw 'Call Start-LakeBuildTests before executing tests.' }
    Assert-LakeSameRecords $Context.TestedFiles @(Get-LakeArtifactRecords $Context.ArtifactDirectory $Context.Names) 'test/artifact'
    if ($Name -cnotin @(Get-LakeRequiredTests $Context.Document.Component) -or $Name -cin @($Context.Results | ForEach-Object { $_.Name })) { throw 'Unknown/duplicate test name.' }
    $folder = Assert-LakeSafePath (Join-Path (Split-Path $Context.ManifestPath -Parent) ('.build-evidence\'+$Context.Document.BuildId))
    [IO.Directory]::CreateDirectory($folder) | Out-Null
    $log = Join-Path $folder ($Name+'.txt')
    $stdout = Join-Path $folder ($Name+'.o')
    $stderr = Join-Path $folder ($Name+'.e')
    foreach ($path in @($log,$stdout,$stderr)) {
        if (Test-Path -LiteralPath $path) { throw 'Existing evidence output preserved; start a new build/test run.' }
    }
    [IO.File]::WriteAllText($log,"Test: $Name`r`n",[Text.UTF8Encoding]::new($false))
    $code = 0
    try {
        $command = Get-Command $Program -CommandType Application,ExternalScript -ErrorAction Stop
        $executable = $command.Source
        $childArguments = @($Arguments)
        if ([IO.Path]::GetExtension($executable) -ieq '.ps1') {
            $childArguments = @('-NoProfile','-NonInteractive','-File',$executable)+$childArguments
            $executable = Join-Path $PSHOME 'powershell.exe'
        }
        $processOptions = @{FilePath=$executable;NoNewWindow=$true;Wait=$true;PassThru=$true;RedirectStandardOutput=$stdout;RedirectStandardError=$stderr;ErrorAction='Stop'}
        if ($childArguments.Count) {
            $processOptions.ArgumentList = (@($childArguments | ForEach-Object { ConvertTo-LakeProcessArgument $_ }) -join ' ')
        }
        # Native stderr is diagnostic data, not a PowerShell terminating error.
        # Separate streams avoid pipe deadlocks and preserve the real exit code.
        $process = Start-Process @processOptions
        try { $code = $process.ExitCode } finally { $process.Dispose() }
        $sink = [IO.File]::Open($log,[IO.FileMode]::Append,[IO.FileAccess]::Write)
        try {
            foreach ($path in @($stdout,$stderr)) {
                $source = [IO.File]::OpenRead($path)
                try { $source.CopyTo($sink) } finally { $source.Dispose() }
                $newline = [Text.Encoding]::ASCII.GetBytes("`r`n")
                $sink.Write($newline,0,$newline.Length)
            }
            $footer = [Text.Encoding]::ASCII.GetBytes("ExitCode: $code`r`n")
            $sink.Write($footer,0,$footer.Length)
        } finally { $sink.Dispose() }
    }
    catch { ($_ | Out-String) | Out-File -LiteralPath $log -Append; $code = 1 }
    Add-LakeBuildTestResult -Context $Context -Name $Name -ExitCode $code -EvidencePath $log
}

function ConvertTo-LakeProcessArgument([AllowEmptyString()][string]$Value) {
    # Windows argv quoting (CRT/CommandLineToArgvW). Start-Process otherwise joins
    # ArgumentList with spaces and loses embedded quotes, empty args and paths.
    if ($Value.Length -and $Value -notmatch '[\s"]') { return $Value }
    $builder = [Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') { [void]$builder.Append(('\' * ($slashes*2+1))) }
        else { [void]$builder.Append(('\' * $slashes)) }
        [void]$builder.Append($character); $slashes = 0
    }
    [void]$builder.Append(('\' * ($slashes*2)))
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Complete-LakeBuildProvenance {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Context,[Parameter(Mandatory)][string]$ArtifactDirectory,
        [string]$PayloadManifestPath,[string]$PayloadHeaderPath,[switch]$RequireTests)
    Assert-LakeBuildContext $Context
    $source = Get-LakeSourceIdentity $Context.RepositoryRoot
    if ($source.Commit -cne $Context.Document.SourceCommit -or $source.Dirty -cne $Context.Document.SourceDirty -or
        $source.Fingerprint -cne $Context.Document.SourceFingerprint) { throw 'Source changed during build/tests. Start a new build.' }
    if ((Get-LakeFileRecord $Context.CompilerPath 'compiler' 1073741824).Sha256 -cne $Context.Document.Toolchain.CompilerSha256) { throw 'Compiler changed during build/tests.' }
    if ($Context.GeneratedHeader -and (Get-LakeFileRecord $Context.GeneratedHeader 'build-stamp').Sha256 -cne $Context.GeneratedHeaderSha256) {
        throw 'The compiled build ID header changed; artifact identity is not attested. Start a new build.'
    }
    $records = @(Get-LakeArtifactRecords $ArtifactDirectory $Context.Names)
    if ($Context.ArtifactDirectory) { Assert-LakeSameRecords $records $Context.TestedFiles 'final/tested artifacts' }
    foreach ($evidence in $Context.Evidence) {
        if ((Get-LakeFileRecord $evidence.Path 'evidence').Sha256 -cne $evidence.Sha256) { throw 'Test evidence changed before finalization.' }
    }
    if ($Context.Document.Component -eq 'Runtime') {
        Assert-LakeSameRecords @($records | Where-Object { $_.Name -cnotmatch '\.(exe|dll)$' }) $Context.Inputs 'runtime/default source files'
    } else {
        if (!$PayloadManifestPath) { $PayloadManifestPath = $Context.PayloadManifestPath }
        if (!$PayloadHeaderPath) { $PayloadHeaderPath = $Context.PayloadHeaderPath }
        if ((Get-LakeFileRecord $PayloadManifestPath 'payload-manifest').Sha256 -cne $Context.Document.PayloadManifestSha256) {
            throw 'Payload manifest changed after setup begin.'
        }
        $payload = Read-LakeJson $PayloadManifestPath
        $runtimeHash = (Get-LakeFileRecord $Context.RuntimeManifestPath 'runtime-manifest').Sha256
        if ($runtimeHash -cne $Context.Document.RuntimeManifestSha256) { throw 'Runtime manifest changed during setup compilation.' }
        Assert-LakePayloadMatchesRuntime $payload $Context.RuntimeDocument $runtimeHash
        if ((Get-LakeFileRecord $PayloadHeaderPath 'header').Sha256 -cne $payload.HeaderSha256) { throw 'Compiled payload header changed.' }
    }
    $Context.Document.Files = $records
    $Context.Document.Tests = [pscustomobject]@{BuildId=$Context.Document.BuildId;FilesSha256=$null;Results=@($Context.Results.ToArray())}
    if ($Context.Results.Count) { $Context.Document.Tests.FilesSha256 = Get-LakeRecordsHash $Context.TestedFiles }
    $Context.Document.CompletedUtc = [DateTime]::UtcNow.ToString('o')
    $Context.Document.State = 'Complete'
    try { Assert-LakeBuildDocument $Context.Document -RequireTests:$RequireTests -AllowUnsignedDevelopment }
    catch { $Context.Document.State = 'Building'; throw }
    Write-LakeJson $Context.ManifestPath $Context.Document
    return $Context.Document
}

function ConvertTo-LakePublicProvenance($Document,[string]$ManifestHash) {
    $copy = [ordered]@{}
    foreach ($property in $Document.PSObject.Properties) {
        if ($property.Name -cne 'Arguments') { $copy[$property.Name] = $property.Value }
    }
    $copy['ManifestSha256'] = $ManifestHash
    return [pscustomobject]$copy
}

function Assert-LakeProvenancePair($Runtime,$Setup,[switch]$Public,[switch]$AllowUnsignedDevelopment) {
    Assert-LakeBuildDocument $Runtime -Public:$Public -RequireTests -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
    Assert-LakeBuildDocument $Setup -Public:$Public -RequireTests -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
    if ($Runtime.Component -cne 'Runtime' -or $Setup.Component -cne 'Setup') { throw 'Expected runtime and setup provenance.' }
    foreach ($field in @('Version','SourceCommit','SourceDirty','SourceFingerprint','AbiVersion','ConfigSchema','Architecture','Configuration','Profiling','DistributionProfile')) {
        if ($Runtime.$field -cne $Setup.$field) { throw "Mixed runtime/setup provenance: $field." }
    }
    if ($Runtime.Configuration -cne 'Release' -or $Runtime.Profiling) { throw 'Only non-profiling Release x86 artifacts are distributable.' }
    if ($Public) {
        if ($Setup.RuntimeManifestSha256 -cne $Runtime.ManifestSha256) { throw 'Setup references another runtime manifest.' }
        Assert-LakePayloadMatchesRuntime $Setup.Payload $Runtime $Runtime.ManifestSha256
    }
}

function Test-LakeReleasePackage {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$PackageDirectory,[string]$Version,[switch]$AllowUnsignedDevelopment)
    $package = Assert-LakeSafePath $PackageDirectory
    $directory = Get-Item -LiteralPath $package -Force -ErrorAction Stop
    if (!$directory.PSIsContainer) { throw 'Package path must be a regular directory.' }
    $manifest = Read-LakeJson (Join-Path $package 'RELEASE-MANIFEST.json')
    # Only the generated representation is public. Besides unexpected fields,
    # this rejects duplicate JSON keys hiding private text discarded by parsing.
    $canonical = ($manifest | ConvertTo-Json -Depth 24)+"`n"
    if ([IO.File]::ReadAllText((Join-Path $package 'RELEASE-MANIFEST.json')) -cne $canonical) { throw 'Release manifest is not canonical generated JSON. Regenerate the package.' }
    Assert-LakeFields $manifest @('Schema','Project','Version','SourceCommit','SourceDirty','RuntimeFiles','Assets','Experimental','Notes','Provenance') 'release manifest'
    if ($manifest.Schema -ne 2 -or $manifest.Project -cne 'lake-controller-windowed-fix' -or $manifest.Version -cnotmatch '^\d+\.\d+\.\d+$' -or
        ($Version -and $manifest.Version -cne $Version) -or $manifest.SourceCommit -cnotmatch '^[a-f0-9]{40}$' -or
        $manifest.SourceDirty -isnot [bool] -or $manifest.Experimental -isnot [bool] -or !$manifest.Experimental) { throw 'Invalid release manifest identity.' }
    if ((@($manifest.Notes) -join "`n") -cne (@(Get-LakeReleaseNotes) -join "`n")) { throw 'Unexpected release notes metadata.' }
    $runtimeNames = @(Get-LakeRuntimeNames)
    if ((@($manifest.RuntimeFiles) -join '|') -cne ($runtimeNames -join '|') -or @($manifest.RuntimeFiles).Count -ne 6) { throw 'Unexpected runtime whitelist.' }
    Assert-LakeFields $manifest.Provenance @('Runtime','Setup') 'release provenance'
    $runtime = $manifest.Provenance.Runtime
    $setup = $manifest.Provenance.Setup
    Assert-LakeProvenancePair $runtime $setup -Public -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
    if ($manifest.Version -cne $runtime.Version -or $manifest.SourceCommit -cne $runtime.SourceCommit -or $manifest.SourceDirty -cne $runtime.SourceDirty) { throw 'Release/build identity mismatch.' }
    $v = $manifest.Version
    $expected = @("LakeWindowedFix-v$v-win32.zip","LakeWindowedFix-Setup-v$v.exe",'SHA256SUMS.txt')
    $allowed = $expected + @('RELEASE-MANIFEST.json')
    $entries = @(Get-ChildItem -LiteralPath $package -Force)
    if ($entries.Count -ne 4 -or @($entries | Where-Object { $_.Name -cnotin $allowed }).Count) { throw 'Package directory is not the exact four-asset whitelist. Unknown files are preserved.' }
    Assert-LakeRecords $manifest.Assets $expected 'release asset'
    $actual = @(Get-LakeArtifactRecords $package $expected)
    Assert-LakeSameRecords $actual $manifest.Assets 'release asset'
    if ($actual[1].Sha256 -cne $setup.Files[0].Sha256 -or $actual[1].Bytes -ne $setup.Files[0].Bytes) { throw 'Setup binary was changed after build/tests.' }
    Assert-LakeX86Pe (Join-Path $package $expected[1])
    $checksumText = (@($actual[0..1] | ForEach-Object { "$($_.Sha256)  $($_.Name)" }) -join "`r`n")+"`r`n"
    if ([IO.File]::ReadAllText((Join-Path $package 'SHA256SUMS.txt')) -cne $checksumText) { throw 'Checksum contents do not match binary assets.' }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $package $expected[0]))
    try {
        $names = @($zip.Entries | ForEach-Object { $_.FullName })
        if ($names.Count -ne 6 -or @($names | Sort-Object -Unique).Count -ne 6 -or @($names | Where-Object { $_ -cnotin $runtimeNames }).Count) { throw 'ZIP contains unexpected files; fixtures/proprietary components are forbidden.' }
        foreach ($record in $runtime.Files) {
            $entry = @($zip.Entries | Where-Object { $_.FullName -ceq $record.Name })[0]
            if ($entry.Length -ne $record.Bytes) { throw 'ZIP payload length mismatch.' }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
            finally { $sha.Dispose(); $stream.Dispose() }
            if ($hash -cne $record.Sha256) { throw 'ZIP payload changed after runtime build/tests.' }
        }
    } finally { $zip.Dispose() }
    return $manifest
}

function Move-LakeReleaseDirectory([string]$Source,[string]$Destination) {
    [IO.Directory]::Move($Source,$Destination)
}

function Publish-LakeReleaseDirectory {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Stage,[Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Version,[switch]$AllowUnsignedDevelopment)
    $stagePath = Assert-LakeSafePath $Stage
    $target = Assert-LakeSafePath $Destination
    $parent = Split-Path $target -Parent
    if ((Split-Path $stagePath -Parent) -cne $parent -or $stagePath -ieq $target) { throw 'Promotion requires distinct sibling directories on the same volume.' }
    $null = Test-LakeReleasePackage $stagePath -Version $Version -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
    $leaf = Split-Path $target -Leaf
    $lockPath = Join-Path $parent ('.'+$leaf+'.release-lock')
    $id = [guid]::NewGuid().ToString('N')
    $backup = Join-Path $parent ('.'+$leaf+'.previous-'+$id)
    $journalPath = Join-Path $parent ('.'+$leaf+'.transaction-'+$id+'.json')
    $lock = [IO.File]::Open($lockPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $journal = [pscustomobject]@{Schema=1;Kind='LakeReleasePromotion';State='Prepared';Destination=$target;Stage=$stagePath;Backup=$backup}
    $oldMoved = $false
    $newMoved = $false
    $rollbackFailed = $false
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($journalPath)
        $lock.Write($bytes,0,$bytes.Length); $lock.Flush($true)
        Write-LakeJson $journalPath $journal
        # Validate under the lock, immediately before moving an entire directory.
        # No per-asset replacements and no recursive cleanup are performed.
        if (Test-Path -LiteralPath $target) {
            $null = Test-LakeReleasePackage $target -Version $Version -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
            Move-LakeReleaseDirectory $target $backup
            $oldMoved = $true
            $journal.State = 'OldMoved'; Write-LakeJson $journalPath $journal
        }
        Move-LakeReleaseDirectory $stagePath $target
        $newMoved = $true
        $journal.State = 'Committed'; Write-LakeJson $journalPath $journal
    } catch {
        $failure = $_
        try {
            if ($newMoved) { Move-LakeReleaseDirectory $target $stagePath; $newMoved = $false }
            if ($oldMoved) { Move-LakeReleaseDirectory $backup $target; $oldMoved = $false }
            $journal.State = 'RolledBack'; Write-LakeJson $journalPath $journal
        } catch {
            $rollbackFailed = $true
            $journal.State = 'RecoveryRequired'
            try { Write-LakeJson $journalPath $journal } catch { }
        }
        if ($rollbackFailed) { throw "Promotion interrupted; recovery required. Preserve lock/journal and directories: $journalPath. Original error: $($failure.Exception.Message)" }
        throw $failure
    } finally {
        $lock.Dispose()
        # On an interrupted process the lock survives: fail closed until the
        # journal is reviewed. Successful rollback/commit releases only our lock.
        if (!$rollbackFailed) { [IO.File]::Delete($lockPath) }
    }
    Write-Host "Release directory promoted as a unit. Journal: $journalPath"
    if (Test-Path -LiteralPath $backup) { Write-Host "Previous verified package preserved: $backup" }
}
