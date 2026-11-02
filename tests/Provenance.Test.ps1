#Requires -Version 5.1
# Synthetic, non-executable PE headers are used ONLY in GUID private sandboxes.
# No real compiler, Lake, installer, registry, Git or network command is run.
[CmdletBinding()]
param([switch]$DefineHelpersOnly)
$ErrorActionPreference = 'Stop'
$provenanceTestRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $provenanceTestRoot 'scripts\Build-Provenance.ps1')

function Enter-LakeProvenanceMock {
    $saved = [pscustomobject]@{
        Alias=(Get-Alias git -Scope Global -ErrorAction SilentlyContinue)
        State=(Get-Variable LakeProvenanceFixture -Scope Global -ErrorAction SilentlyContinue)
        ExitCode=(Get-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue)
        FunctionName=('Invoke-LakeProvenanceMock-'+[guid]::NewGuid().ToString('N'))
    }
    Set-Item -Path ('Function:global:'+$saved.FunctionName) -Value {
        $global:LASTEXITCODE = 0
        $state = $global:LakeProvenanceFixture
        if (!$state -or $args.Count -lt 3 -or $args[0] -ne '-C' -or $args[1] -ine $state.Root) { throw 'Unexpected mock Git root. Real Git is forbidden.' }
        $operation = @($args | Select-Object -Skip 2) -join ' '
        [void]$state.Calls.Add($operation)
        switch ($operation) {
            'rev-parse HEAD' { return $state.Head }
            'status --porcelain --untracked-files=normal' { if ($state.Dirty) { ' M src/product_version.h' }; return }
            'ls-files --cached --others --exclude-standard' { return $state.Inventory }
            default { throw "Unexpected mock Git operation: $operation. No remote/mutation is allowed." }
        }
    }
    Set-Alias -Name git -Value $saved.FunctionName -Scope Global -Force
    return $saved
}

function Exit-LakeProvenanceMock($Saved) {
    if ($Saved.Alias) { Set-Alias -Name git -Value $Saved.Alias.Definition -Scope Global -Force }
    else { Remove-Item Alias:git -ErrorAction SilentlyContinue }
    Remove-Item -Path ('Function:global:'+$Saved.FunctionName) -ErrorAction SilentlyContinue
    if ($Saved.State) { Set-Variable LakeProvenanceFixture -Scope Global -Value $Saved.State.Value }
    else { Remove-Variable LakeProvenanceFixture -Scope Global -ErrorAction SilentlyContinue }
    if ($Saved.ExitCode) { Set-Variable LASTEXITCODE -Scope Global -Value $Saved.ExitCode.Value }
    else { Remove-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue }
}

function Write-LakeSyntheticPe([string]$Path,[string]$Identity) {
    $bytes = [byte[]]::new(256)
    $bytes[0]=0x4d; $bytes[1]=0x5a; $bytes[60]=64
    $bytes[64]=0x50; $bytes[65]=0x45; $bytes[68]=0x4c; $bytes[69]=1
    $label = [Text.Encoding]::ASCII.GetBytes('TEST FIXTURE - NOT EXECUTABLE: '+$Identity)
    [Array]::Copy($label,0,$bytes,80,[Math]::Min($label.Length,176))
    [IO.File]::WriteAllBytes($Path,$bytes)
}

function Add-LakeSyntheticTests($Context,[string]$Folder) {
    [IO.Directory]::CreateDirectory($Folder) | Out-Null
    foreach ($name in @(Get-LakeRequiredTests $Context.Document.Component)) {
        $path = Join-Path $Folder ($name+'.txt')
        [IO.File]::WriteAllText($path,'SYNTHETIC test result for sandbox guard tests; no executable ran.')
        Add-LakeBuildTestResult -Context $Context -Name $name -ExitCode 0 -EvidencePath $path
    }
}

function New-LakeProvenanceFixture([string]$Parent,[switch]$Dirty,[switch]$RuntimeOnly) {
    $folder = Join-Path $Parent ('fixture-'+[guid]::NewGuid().ToString('N'))
    foreach ($child in @('scripts','src','config','runtime','setup','build','logs')) { [IO.Directory]::CreateDirectory((Join-Path $folder $child)) | Out-Null }
    foreach ($script in @('Build-Provenance.ps1','New-InstallerPayload.ps1','New-ReleasePackage.ps1','Test-Candidate.ps1','Publish-Release.ps1')) {
        [IO.File]::Copy((Join-Path $provenanceTestRoot ('scripts\'+$script)),(Join-Path $folder ('scripts\'+$script)))
    }
    [IO.File]::WriteAllText((Join-Path $folder 'src\product_version.h'),"#define LAKE_FIX_VERSION `"0.7.0`"`n#define LAKE_FIX_ABI_VERSION 2`n#define LAKE_FIX_CONFIG_SCHEMA 1`n")
    [IO.File]::WriteAllText((Join-Path $folder 'config\LakeWindowedFix.ini'),"[synthetic-defaults]`nValue=1`n")
    [IO.File]::WriteAllText((Join-Path $folder 'LICENSE'),'SYNTHETIC MIT fixture; not a distributable product.')
    [IO.File]::WriteAllText((Join-Path $folder 'scripts\Measure-LakeResources.ps1'),'# synthetic support fixture')
    [IO.File]::WriteAllText((Join-Path $folder 'scripts\LakeResourceMetrics.psm1'),'# synthetic support fixture')
    [IO.File]::WriteAllText((Join-Path $folder 'mock-compiler.ps1'),'# synthetic compiler identity; never executed')
    $global:LakeProvenanceFixture = @{
        Root=$folder;Head=('a'*40);Dirty=[bool]$Dirty
        Inventory=@('src/product_version.h','config/LakeWindowedFix.ini','LICENSE','scripts/Measure-LakeResources.ps1','scripts/LakeResourceMetrics.psm1')
        Calls=[Collections.Generic.List[string]]::new()
    }
    $runtime = Join-Path $folder 'runtime'
    $setup = Join-Path $folder 'setup'
    $common = @{RepositoryRoot=$folder;Version='0.7.0';Toolchain='Zig';ToolchainVersion='0.14.1';CompilerPath=(Join-Path $folder 'mock-compiler.ps1');DistributionProfile='DevelopmentUnsigned'}
    $context = Start-LakeBuildProvenance @common -Component Runtime -ManifestPath (Join-Path $runtime 'BUILD-PROVENANCE.json') -GeneratedHeader (Join-Path $folder 'build\lake_build_id.h') -BuildArguments @('-O2','synthetic-private-argument')
    foreach ($name in @('LakeWindowedLauncher.exe','LakeWindowedFix.dll')) { Write-LakeSyntheticPe (Join-Path $runtime $name) ($context.Document.BuildId+$name) }
    foreach ($entry in @(@('config\LakeWindowedFix.ini','LakeWindowedFix.ini'),@('LICENSE','LICENSE'),@('scripts\Measure-LakeResources.ps1','Measure-LakeResources.ps1'),@('scripts\LakeResourceMetrics.psm1','LakeResourceMetrics.psm1'))) {
        [IO.File]::Copy((Join-Path $folder $entry[0]),(Join-Path $runtime $entry[1]))
    }
    $fixture = [pscustomobject]@{Root=$folder;Runtime=$runtime;Setup=$setup;Context=$context;Common=$common;SetupContext=$null;Payload=(Join-Path $folder 'build\installer_payload.h');Package=(Join-Path $folder 'releases\candidate')}
    if ($RuntimeOnly) { return $fixture }
    Start-LakeBuildTests $context $runtime
    Add-LakeSyntheticTests $context (Join-Path $folder 'logs\runtime')
    $null = Complete-LakeBuildProvenance $context $runtime -RequireTests
    & (Join-Path $folder 'scripts\New-InstallerPayload.ps1') -RuntimeDirectory $runtime -OutputHeader $fixture.Payload -AllowUnsignedDevelopment | Out-Null
    $setupContext = Start-LakeBuildProvenance @common -Component Setup -ManifestPath (Join-Path $setup 'BUILD-PROVENANCE.json') -RuntimeManifestPath (Join-Path $runtime 'BUILD-PROVENANCE.json') -PayloadManifestPath ($fixture.Payload+'.manifest.json')
    Write-LakeSyntheticPe (Join-Path $setup 'LakeWindowedFix-Setup-v0.7.0.exe') $setupContext.Document.BuildId
    Start-LakeBuildTests $setupContext $setup
    Add-LakeSyntheticTests $setupContext (Join-Path $folder 'logs\setup')
    $null = Complete-LakeBuildProvenance $setupContext $setup -RequireTests
    $fixture.SetupContext = $setupContext
    return $fixture
}

function Copy-LakeFixtureOutputs($Fixture,[string]$Parent) {
    $folder = Join-Path $Parent ('copy-'+[guid]::NewGuid().ToString('N'))
    foreach ($component in @('Runtime','Setup')) {
        $destination = Join-Path $folder $component
        [IO.Directory]::CreateDirectory($destination) | Out-Null
        $doc = Read-LakeJson (Join-Path $Fixture.$component 'BUILD-PROVENANCE.json')
        foreach ($name in @($doc.Files.Name)+@('BUILD-PROVENANCE.json')) { [IO.File]::Copy((Join-Path $Fixture.$component $name),(Join-Path $destination $name)) }
    }
    [pscustomobject]@{Root=$folder;Runtime=(Join-Path $folder 'Runtime');Setup=(Join-Path $folder 'Setup');Package=(Join-Path $folder 'releases\candidate')}
}

function New-LakeFixturePackage($Fixture,[switch]$NoUnsignedOptIn) {
    & (Join-Path $provenanceTestRoot 'scripts\New-ReleasePackage.ps1') -RuntimeDirectory $Fixture.Runtime -InstallerDirectory $Fixture.Setup -OutputDirectory $Fixture.Package -AllowUnsignedDevelopment:(!$NoUnsignedOptIn) | Out-Null
}

function Copy-LakeFixturePackage([string]$Source,[string]$Destination) {
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    $doc = Read-LakeJson (Join-Path $Source 'RELEASE-MANIFEST.json')
    foreach ($name in @($doc.Assets.Name)+@('RELEASE-MANIFEST.json')) { [IO.File]::Copy((Join-Path $Source $name),(Join-Path $Destination $name)) }
}

if ($DefineHelpersOnly) { return }
$sandbox = Join-Path $provenanceTestRoot ('private\provenance-tests\'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($sandbox) | Out-Null
$savedMock = Enter-LakeProvenanceMock
$script:provenancePassed = 0
$script:provenanceFailed = 0
function Require-Provenance([bool]$Condition,[string]$Message) { if (!$Condition) { throw $Message } }
function Reject-Provenance([scriptblock]$Action) {
    $rejected = $false
    try { & $Action *> $null } catch { $rejected = $true }
    Require-Provenance $rejected 'Unsafe fixture unexpectedly passed.'
}
function Case-Provenance([string]$Name,[scriptblock]$Action) {
    try { & $Action; $script:provenancePassed++; Write-Host "PASS: $Name" }
    catch { $script:provenanceFailed++; Write-Host "FAIL: $Name - $($_.Exception.Message)"; Write-Host $_.ScriptStackTrace }
}
try {
    $base = New-LakeProvenanceFixture $sandbox
    Case-Provenance 'Begin/finalize captures product, toolchain, arguments, immutable build ID and exact tests' {
        $m = Read-LakeJson (Join-Path $base.Runtime 'BUILD-PROVENANCE.json')
        Assert-LakeBuildDocument $m -RequireTests -AllowUnsignedDevelopment
        Require-Provenance ($m.Version -eq '0.7.0' -and $m.AbiVersion -eq 2 -and $m.ConfigSchema -eq 1 -and $m.Arguments[1] -eq 'synthetic-private-argument') 'Missing build metadata.'
        Require-Provenance ([IO.File]::ReadAllText((Join-Path $base.Root 'build\lake_build_id.h')).Contains('#define LAKE_FIX_BUILD_ID "'+$m.BuildId+'"')) 'Generated build ID does not match provenance.'
    }
    Case-Provenance 'Packaging an old dist never relabels it with current HEAD/defaults/version' {
        $global:LakeProvenanceFixture.Head = 'b'*40
        [IO.File]::WriteAllText((Join-Path $base.Root 'src\product_version.h'),'current source metadata deliberately unavailable')
        [IO.File]::WriteAllText((Join-Path $base.Root 'config\LakeWindowedFix.ini'),'current edited defaults')
        $before = $global:LakeProvenanceFixture.Calls.Count
        New-LakeFixturePackage $base
        $m = Read-LakeJson (Join-Path $base.Package 'RELEASE-MANIFEST.json')
        Require-Provenance ($m.SourceCommit -eq ('a'*40) -and $m.Version -eq '0.7.0' -and $global:LakeProvenanceFixture.Calls.Count -eq $before) 'Packager consulted/relabelled current source.'
        Require-Provenance (![IO.File]::ReadAllText((Join-Path $base.Package 'RELEASE-MANIFEST.json')).Contains('synthetic-private-argument')) 'Private compiler argument leaked.'
    }
    Case-Provenance 'Local candidate validation never calls Git/gh or changes a tag' {
        $before = $global:LakeProvenanceFixture.Calls.Count
        & (Join-Path $provenanceTestRoot 'scripts\Test-Candidate.ps1') -PackageDirectory $base.Package -AllowUnsignedDevelopment *> $null
        Require-Provenance ($global:LakeProvenanceFixture.Calls.Count -eq $before) 'Candidate used Git.'
    }
    Case-Provenance 'Unsigned package and candidate require explicit opt-in' {
        $f = Copy-LakeFixtureOutputs $base $sandbox
        Reject-Provenance { New-LakeFixturePackage $f -NoUnsignedOptIn }
        Reject-Provenance { & (Join-Path $provenanceTestRoot 'scripts\Test-Candidate.ps1') -PackageDirectory $base.Package }
        Reject-Provenance { & (Join-Path $provenanceTestRoot 'scripts\New-InstallerPayload.ps1') -RuntimeDirectory $base.Runtime -OutputHeader (Join-Path $sandbox 'unsigned.h') }
    }
    foreach ($component in @('Runtime','Setup')) {
        Case-Provenance "Missing $component provenance cannot stamp existing binaries" {
            $f = Copy-LakeFixtureOutputs $base $sandbox
            [IO.File]::Move((Join-Path $f.$component 'BUILD-PROVENANCE.json'),(Join-Path $f.$component 'saved.json'))
            Reject-Provenance { New-LakeFixturePackage $f }
        }
    }
    foreach ($component in @('Runtime','Setup')) {
        Case-Provenance "Missing/incompatible $component test evidence is rejected" {
            foreach ($kind in @('missing','incompatible','failed')) {
                $f = Copy-LakeFixtureOutputs $base $sandbox
                $path = Join-Path $f.$component 'BUILD-PROVENANCE.json'
                $m = Read-LakeJson $path
                if ($kind -eq 'missing') { $m.Tests.Results = @() }
                if ($kind -eq 'incompatible') { $m.Tests.FilesSha256 = '0'*64 }
                if ($kind -eq 'failed') { $m.Tests.Results[0].ExitCode = 7 }
                Write-LakeJson $path $m
                Reject-Provenance { New-LakeFixturePackage $f }
            }
        }
    }
    foreach ($component in @('Runtime','Setup')) {
        Case-Provenance "Changed $component binary is rejected" {
            $f = Copy-LakeFixtureOutputs $base $sandbox
            $name = if ($component -eq 'Runtime') { 'LakeWindowedFix.dll' } else { 'LakeWindowedFix-Setup-v0.7.0.exe' }
            Write-LakeSyntheticPe (Join-Path $f.$component $name) 'changed bytes'
            Reject-Provenance { New-LakeFixturePackage $f }
        }
    }
    Case-Provenance 'Mixed runtime manifest/payload identity is rejected even when binaries are unchanged' {
        $f = Copy-LakeFixtureOutputs $base $sandbox
        $path = Join-Path $f.Setup 'BUILD-PROVENANCE.json'
        $m = Read-LakeJson $path; $m.Payload.RuntimeBuildId = 'f'*32
        $m.PayloadManifestSha256 = Get-LakeTextHash (($m.Payload | ConvertTo-Json -Depth 24)+"`n")
        Write-LakeJson $path $m
        Reject-Provenance { New-LakeFixturePackage $f }
    }
    Case-Provenance 'Debug/profiling artifacts cannot become installer releases' {
        foreach ($kind in @('Debug','Profile')) {
            $f = Copy-LakeFixtureOutputs $base $sandbox
            $path = Join-Path $f.Runtime 'BUILD-PROVENANCE.json'
            $m = Read-LakeJson $path
            if ($kind -eq 'Debug') { $m.Configuration = 'Debug' } else { $m.Profiling = $true }
            Write-LakeJson $path $m
            Reject-Provenance { New-LakeFixturePackage $f }
        }
    }
    Case-Provenance 'Unknown dist files/fixtures are never included in the exact six-file ZIP' {
        $f = Copy-LakeFixtureOutputs $base $sandbox
        [IO.File]::WriteAllText((Join-Path $f.Runtime 'Contour.dll'),'synthetic forbidden component')
        [IO.File]::WriteAllText((Join-Path $f.Runtime 'private-log.txt'),'synthetic private data')
        New-LakeFixturePackage $f
        $null = Test-LakeReleasePackage $f.Package -AllowUnsignedDevelopment
    }
    Case-Provenance 'Changed source with the same dirty flag prevents finalization' {
        $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly -Dirty
        [IO.File]::WriteAllText((Join-Path $f.Root 'config\LakeWindowedFix.ini'),'changed twice but still dirty')
        Reject-Provenance { Complete-LakeBuildProvenance $f.Context $f.Runtime }
    }
    Case-Provenance 'SkipTests is recorded but never becomes distributable evidence' {
        $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly
        $m = Complete-LakeBuildProvenance $f.Context $f.Runtime
        Require-Provenance ($m.Tests.Results.Count -eq 0 -and $null -eq $m.Tests.FilesSha256) 'Untested build claims tests.'
        Reject-Provenance { Assert-LakeBuildDocument $m -RequireTests -AllowUnsignedDevelopment }
        Reject-Provenance { & (Join-Path $f.Root 'scripts\New-InstallerPayload.ps1') -RuntimeDirectory $f.Runtime -OutputHeader $f.Payload -AllowUnsignedDevelopment }
    }
    Case-Provenance 'Starting an incremental build invalidates previous completion immediately' {
        $f = New-LakeProvenanceFixture $sandbox
        $common = $f.Common
        $null = Start-LakeBuildProvenance @common -Component Runtime -ManifestPath (Join-Path $f.Runtime 'BUILD-PROVENANCE.json')
        Reject-Provenance { New-LakeFixturePackage $f }
    }
    Case-Provenance 'Actual compiler command/quoting is captured before tests and its digest is consistent' {
        $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly
        Add-LakeBuildCommand $f.Context 'C:\tool path\compiler.exe' @('-O2','source path.cpp','-DVALUE="exact"')
        $pending = Read-LakeJson $f.Context.ManifestPath
        Require-Provenance ($pending.Arguments[-1] -ceq '"C:\tool path\compiler.exe" -O2 "source path.cpp" "-DVALUE=\"exact\""') 'Actual command argv was not preserved.'
        Require-Provenance ($pending.ArgumentsSha256 -ceq (Get-LakeTextHash (ConvertTo-Json -InputObject @($pending.Arguments) -Compress))) 'Command argument digest is stale.'
        Reject-Provenance { Add-LakeBuildCommand $f.Context 'compiler.exe' @('-O2','') }
        Start-LakeBuildTests $f.Context $f.Runtime
        Reject-Provenance { Add-LakeBuildCommand $f.Context 'compiler.exe' @('too-late.cpp') }
    }
    Case-Provenance 'Unknown provenance metadata is preserved rather than overwritten by begin' {
        $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly
        $path = Join-Path $f.Runtime 'BUILD-PROVENANCE.json'
        $m = Read-LakeJson $path
        $m | Add-Member -NotePropertyName UnknownUserWork -NotePropertyValue 'preserve this file'
        Write-LakeJson $path $m
        $before = (Get-FileHash -LiteralPath $path).Hash
        $common = $f.Common
        Reject-Provenance { Start-LakeBuildProvenance @common -Component Runtime -ManifestPath $path }
        Require-Provenance ((Get-FileHash -LiteralPath $path).Hash -eq $before) 'Unknown provenance was overwritten.'
    }
    Case-Provenance 'Signed profile fails closed without claiming a certificate' {
        $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly
        $common = @{}+$f.Common; $common.DistributionProfile = 'Signed'
        Reject-Provenance { Start-LakeBuildProvenance @common -Component Runtime -ManifestPath (Join-Path $f.Root 'signed.json') }
        Require-Provenance (!(Test-Path -LiteralPath (Join-Path $f.Root 'signed.json'))) 'Signed intent was invented.'
    }
    Case-Provenance 'Changing artifacts during tests cannot reuse the test snapshot' {
        $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly
        Start-LakeBuildTests $f.Context $f.Runtime
        Write-LakeSyntheticPe (Join-Path $f.Runtime 'LakeWindowedFix.dll') 'changed during tests'
        Reject-Provenance { Add-LakeSyntheticTests $f.Context (Join-Path $f.Root 'logs') }
    }
    Case-Provenance 'Changed evidence, compiler or generated build ID prevents completion' {
        foreach ($kind in @('evidence','compiler','stamp')) {
            $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly
            Start-LakeBuildTests $f.Context $f.Runtime
            Add-LakeSyntheticTests $f.Context (Join-Path $f.Root 'logs')
            $path = if ($kind -eq 'compiler') { Join-Path $f.Root 'mock-compiler.ps1' }
                elseif ($kind -eq 'stamp') { $f.Context.GeneratedHeader } else { $f.Context.Evidence[0].Path }
            [IO.File]::WriteAllText($path,'changed after tests')
            Reject-Provenance { Complete-LakeBuildProvenance $f.Context $f.Runtime -RequireTests }
        }
    }
    Case-Provenance 'Stale log from a previous test run is rejected' {
        $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly
        Start-LakeBuildTests $f.Context $f.Runtime
        $log = Join-Path $f.Root 'logs\old.txt'; [IO.File]::WriteAllText($log,'old log')
        [IO.File]::SetLastWriteTimeUtc($log,[DateTime]::UtcNow.AddDays(-1))
        Reject-Provenance { Add-LakeBuildTestResult $f.Context 'scale_geometry' 0 $log }
    }
    Case-Provenance 'Invoke-LakeBuildTest captures subprocess exit/stderr without running generated binaries' {
        $f = New-LakeProvenanceFixture $sandbox -RuntimeOnly
        Start-LakeBuildTests $f.Context $f.Runtime
        $mock = Join-Path $f.Root 'mock-test.ps1'
        [IO.File]::WriteAllText($mock,"[Console]::Error.WriteLine('informational stderr, not a failure'); 'synthetic test output'; exit 0")
        Invoke-LakeBuildTest $f.Context 'scale_geometry' $mock
        Require-Provenance ($f.Context.Results.Count -eq 1 -and $f.Context.Results[0].ExitCode -eq 0) 'Mock test result not captured.'
        [IO.File]::WriteAllText($mock,"'synthetic test failure'; exit 9")
        Reject-Provenance { Invoke-LakeBuildTest $f.Context 'ui_quality_policy' $mock }
        Require-Provenance ($f.Context.Results[1].ExitCode -eq 9) 'Child exit code was reinterpreted as a PowerShell error.'
    }
    Case-Provenance 'Changed payload header after setup begin prevents setup finalization' {
        $f = New-LakeProvenanceFixture $sandbox
        $common = $f.Common
        $ctx = Start-LakeBuildProvenance @common -Component Setup -ManifestPath (Join-Path $f.Setup 'BUILD-PROVENANCE.json') -RuntimeManifestPath (Join-Path $f.Runtime 'BUILD-PROVENANCE.json') -PayloadManifestPath ($f.Payload+'.manifest.json')
        [IO.File]::AppendAllText($f.Payload,"`n// changed after begin")
        Reject-Provenance { Complete-LakeBuildProvenance $ctx $f.Setup }
    }
    Case-Provenance 'Dirty development candidate is allowed locally without publication approval' {
        $f = New-LakeProvenanceFixture $sandbox -Dirty
        New-LakeFixturePackage $f
        & (Join-Path $provenanceTestRoot 'scripts\Test-Candidate.ps1') -PackageDirectory $f.Package -AllowUnsignedDevelopment *> $null
    }
    Case-Provenance 'Unknown output files and altered old assets are preserved, never partially overwritten' {
        foreach ($kind in @('unknown','altered')) {
            $f = Copy-LakeFixtureOutputs $base $sandbox
            Copy-LakeFixturePackage $base.Package $f.Package
            $path = if ($kind -eq 'unknown') { Join-Path $f.Package 'unowned.txt' } else { Join-Path $f.Package 'SHA256SUMS.txt' }
            [IO.File]::WriteAllText($path,'must be preserved')
            $before = (Get-FileHash -LiteralPath $path).Hash
            Reject-Provenance { New-LakeFixturePackage $f }
            Require-Provenance ((Get-FileHash -LiteralPath $path).Hash -eq $before) 'Unknown/altered file was overwritten.'
        }
    }
    Case-Provenance 'Directory promotion rolls back on a second-rename failure, retaining complete old release' {
        $parent = Join-Path $sandbox ('rollback-'+[guid]::NewGuid().ToString('N'))
        $target = Join-Path $parent 'candidate'; $stage = Join-Path $parent '.stage'
        Copy-LakeFixturePackage $base.Package $target; Copy-LakeFixturePackage $base.Package $stage
        $oldHash = (Get-FileHash (Join-Path $target 'RELEASE-MANIFEST.json')).Hash
        $originalMove = ${function:Move-LakeReleaseDirectory}
        $script:promotionMoves = 0
        function Move-LakeReleaseDirectory([string]$Source,[string]$Destination) {
            $script:promotionMoves++
            if ($script:promotionMoves -eq 2) { throw 'Injected sandbox rename failure' }
            [IO.Directory]::Move($Source,$Destination)
        }
        try { Reject-Provenance { Publish-LakeReleaseDirectory $stage $target '0.7.0' -AllowUnsignedDevelopment } }
        finally { Set-Item Function:Move-LakeReleaseDirectory $originalMove }
        $null = Test-LakeReleasePackage $target -AllowUnsignedDevelopment
        Require-Provenance ((Get-FileHash (Join-Path $target 'RELEASE-MANIFEST.json')).Hash -eq $oldHash -and (Test-Path -LiteralPath $stage)) 'Rollback did not restore the entire release.'
        $journal = @(Get-ChildItem -LiteralPath $parent -Force -Filter '*.json')[0]
        Require-Provenance ((Read-LakeJson $journal.FullName).State -eq 'RolledBack') 'Rollback journal absent.'
    }
    Case-Provenance 'Successful directory promotion retains backup and committed journal' {
        $f = Copy-LakeFixtureOutputs $base $sandbox
        Copy-LakeFixturePackage $base.Package $f.Package
        New-LakeFixturePackage $f
        $parent = Split-Path $f.Package -Parent
        Require-Provenance (@(Get-ChildItem -LiteralPath $parent -Force -Directory -Filter '.candidate.previous-*').Count -eq 1) 'Verified backup was not preserved.'
        $journal = @(Get-ChildItem -LiteralPath $parent -Force -Filter '*.json')[0]
        Require-Provenance ((Read-LakeJson $journal.FullName).State -eq 'Committed') 'Commit journal absent.'
    }
    Case-Provenance 'Failed rollback retains complete backup/stage plus a blocking recovery journal and lock' {
        $parent = Join-Path $sandbox ('recovery-'+[guid]::NewGuid().ToString('N'))
        $target = Join-Path $parent 'candidate'; $stage = Join-Path $parent '.stage'
        Copy-LakeFixturePackage $base.Package $target; Copy-LakeFixturePackage $base.Package $stage
        $originalMove = ${function:Move-LakeReleaseDirectory}
        $script:promotionMoves = 0
        function Move-LakeReleaseDirectory([string]$Source,[string]$Destination) {
            $script:promotionMoves++
            if ($script:promotionMoves -ge 2) { throw 'Injected rename and rollback failures' }
            [IO.Directory]::Move($Source,$Destination)
        }
        try { Reject-Provenance { Publish-LakeReleaseDirectory $stage $target '0.7.0' -AllowUnsignedDevelopment } }
        finally { Set-Item Function:Move-LakeReleaseDirectory $originalMove }
        $journal = Read-LakeJson (@(Get-ChildItem -LiteralPath $parent -Force -Filter '*.json')[0].FullName)
        Require-Provenance ($journal.State -eq 'RecoveryRequired' -and (Test-Path -LiteralPath (Join-Path $parent '.candidate.release-lock')) -and !(Test-Path -LiteralPath $target)) 'Interrupted transaction did not fail closed.'
        $null = Test-LakeReleasePackage $journal.Backup -AllowUnsignedDevelopment
        $null = Test-LakeReleasePackage $stage -AllowUnsignedDevelopment
        Reject-Provenance { Publish-LakeReleaseDirectory $stage $target '0.7.0' -AllowUnsignedDevelopment }
    }
    Case-Provenance 'An existing promotion lock fails closed and is not overwritten' {
        $parent = Join-Path $sandbox ('locked-'+[guid]::NewGuid().ToString('N'))
        $target = Join-Path $parent 'candidate'; $stage = Join-Path $parent '.stage'
        Copy-LakeFixturePackage $base.Package $stage
        $lockPath = Join-Path $parent '.candidate.release-lock'
        [IO.File]::WriteAllText($lockPath,'foreign/stale lock')
        Reject-Provenance { Publish-LakeReleaseDirectory $stage $target '0.7.0' -AllowUnsignedDevelopment }
        Require-Provenance ([IO.File]::ReadAllText($lockPath) -ceq 'foreign/stale lock') 'Existing lock was changed.'
    }
} finally { Exit-LakeProvenanceMock $savedMock }
Write-Host "Provenance guards: $script:provenancePassed passed, $script:provenanceFailed failed. Synthetic fixtures remain only at $sandbox"
if ($script:provenanceFailed) { throw 'Provenance guard tests failed.' }
