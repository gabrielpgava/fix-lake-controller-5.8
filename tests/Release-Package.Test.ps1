#Requires -Version 5.1
# All git/gh operations are mocks. Only GUID-named private fixtures are written;
# no remote, tag, release, registry, real install or Lake process is touched.
[CmdletBinding()]
param([string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Provenance.Test.ps1') -DefineHelpersOnly
$sandbox = Join-Path $root ('private\release-tests\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$sandbox\scripts" | Out-Null
if (!$PackageDirectory) {
    $savedFixtureMock = Enter-LakeProvenanceMock
    try {
        $fixture = New-LakeProvenanceFixture $sandbox
        New-LakeFixturePackage $fixture
        $PackageDirectory = $fixture.Package
    } finally { Exit-LakeProvenanceMock $savedFixtureMock }
}
$inputPackage = (Resolve-Path -LiteralPath $PackageDirectory).Path
$inputManifest = Test-LakeReleasePackage $inputPackage -AllowUnsignedDevelopment
$version = $inputManifest.Version
[IO.Directory]::CreateDirectory((Join-Path $sandbox 'src')) | Out-Null
[IO.File]::WriteAllText((Join-Path $sandbox 'src\product_version.h'),"#define LAKE_FIX_VERSION `"$version`"`n#define LAKE_FIX_ABI_VERSION $($inputManifest.Provenance.Runtime.AbiVersion)`n#define LAKE_FIX_CONFIG_SCHEMA $($inputManifest.Provenance.Runtime.ConfigSchema)`n")
foreach ($name in @('Publish-Release.ps1','Build-Provenance.ps1','Test-Candidate.ps1')) {
    [IO.File]::Copy((Join-Path $root ('scripts\'+$name)),(Join-Path $sandbox ('scripts\'+$name)),$false)
}
$mockGh = Join-Path $sandbox 'mock-gh.ps1'
[IO.File]::WriteAllText($mockGh,@'
$global:LASTEXITCODE = 0
[void]$global:LakeReleaseFixture.Gh.Add(($args -join '|'))
if ($args[0] -eq 'auth' -and $args[1] -eq 'status') { return }
if ($args[0] -eq 'release' -and $args[1] -eq 'list') {
    if ($global:LakeReleaseFixture.ExistingRelease) { '[{"tagName":"'+$global:LakeReleaseFixture.Tag+'"}]' }
    else { '[]' }
    return
}
if ($args[0] -eq 'release' -and $args[1] -eq 'create') { return }
throw 'Unexpected mock gh command. No network operation is permitted.'
'@,[Text.Encoding]::ASCII)
$oldAlias = Get-Alias git -Scope Global -ErrorAction SilentlyContinue
$oldState = Get-Variable LakeReleaseFixture -Scope Global -ErrorAction SilentlyContinue
$oldHost = $env:GH_HOST
$functionName = 'Invoke-LakeReleaseFixtureGit-'+[guid]::NewGuid().ToString('N')
Set-Item -Path "Function:global:$functionName" -Value {
    $global:LASTEXITCODE = 0
    $state = $global:LakeReleaseFixture
    if ($args.Count -lt 3 -or $args[0] -ne '-C') { throw 'Unexpected fixture Git invocation.' }
    $operation = @($args | Select-Object -Skip 2) -join ' '
    [void]$state.Git.Add($operation)
    if ($operation -eq 'rev-parse HEAD') { return $state.Head }
    if ($operation -eq 'status --porcelain --untracked-files=normal') { if ($state.Dirty) { ' M source.cpp' }; return }
    if ($operation -eq 'remote get-url origin') { return 'https://github.com/example/lake-test.git' }
    if ($operation -eq 'remote get-url --push --all origin') { return $state.PushUrls }
    if ($operation -eq 'ls-remote --heads origin refs/heads/main') { return $state.RemoteHead+"`trefs/heads/main" }
    if ($operation -eq ('tag --list '+$state.Tag)) { if ($state.LocalTag) { return $state.Tag }; return }
    if ($operation -eq ('rev-parse '+$state.Tag+'^{commit}')) { return $state.LocalTag }
    if ($operation.StartsWith('ls-remote --tags origin ')) {
        if ($state.RemoteTag) { return $state.RemoteTag+"`trefs/tags/"+$state.Tag }; return
    }
    if ($operation.StartsWith('tag -a ') -or $operation.StartsWith('push origin refs/tags/')) {
        if (!$state.AllowWrites) { throw 'Unexpected mock write; real Git is never invoked.' }
        [void]$state.Writes.Add($operation)
        return
    }
    throw "Unexpected mock Git operation: $operation"
}
Set-Alias -Name git -Value $functionName -Scope Global -Force
$script:passed = 0
$script:failed = 0
$script:index = 0
function Require([bool]$Condition,[string]$Message) { if (!$Condition) { throw $Message } }
function Case([string]$Label,[scriptblock]$Action) {
    $script:index++
    $package = Join-Path $sandbox "case-$script:index"
    New-Item -ItemType Directory -Path $package | Out-Null
    foreach ($assetName in @($inputManifest.Assets.Name)+@('RELEASE-MANIFEST.json')) {
        [IO.File]::Copy((Join-Path $inputPackage $assetName),(Join-Path $package $assetName),$false)
    }
    $global:LakeReleaseFixture = @{
        Head=('a'*40);RemoteHead=('a'*40);Tag="v$version";Dirty=$false;LocalTag=$null;RemoteTag=$null
        PushUrls=@('https://github.com/example/lake-test.git');AllowWrites=$false;ExistingRelease=$false
        Git=[Collections.Generic.List[string]]::new();Gh=[Collections.Generic.List[string]]::new();Writes=[Collections.Generic.List[string]]::new()
    }
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $package 'RELEASE-MANIFEST.json') | ConvertFrom-Json
    $manifest.SourceCommit = 'a'*40
    $manifest.SourceDirty = $false
    foreach ($component in @('Runtime','Setup')) {
        $manifest.Provenance.$component.SourceCommit = 'a'*40
        $manifest.Provenance.$component.SourceDirty = $false
    }
    $manifest.Provenance.Setup.Payload.SourceCommit = 'a'*40
    $manifest.Provenance.Setup.Payload.SourceDirty = $false
    $manifest.Provenance.Setup.PayloadManifestSha256 = Get-LakeTextHash (($manifest.Provenance.Setup.Payload | ConvertTo-Json -Depth 24)+"`n")
    Save-Manifest $package $manifest
    try {
        & $Action $package $manifest
        Write-Host "PASS: $Label"
        $script:passed++
    } catch {
        Write-Host "FAIL: $Label - $($_.Exception.Message)"
        $script:failed++
    }
}
function Save-Manifest([string]$Package,[object]$Manifest) {
    Write-LakeJson (Join-Path $Package 'RELEASE-MANIFEST.json') $Manifest
}
function Reject([string]$Package) {
    $rejected = $false
    try { & "$sandbox\scripts\Publish-Release.ps1" -Version $version -PackageDirectory $Package -GhPath $mockGh -AllowUnsignedDevelopment -DryRun *> $null }
    catch { $rejected = $true }
    Require $rejected 'Publication guard unexpectedly approved the fixture.'
    Require ($global:LakeReleaseFixture.Writes.Count -eq 0 -and $global:LakeReleaseFixture.Gh.Count -eq 0) 'Rejected dry-run tried to write Git or invoke gh.'
}
try {
    $env:GH_HOST = 'unrelated.invalid'
    Case 'Valid dry-run uses only mock reads; no GitHub CLI or mutation' {
        param($p,$m)
        & "$sandbox\scripts\Publish-Release.ps1" -Version $version -PackageDirectory $p -GhPath $mockGh -AllowUnsignedDevelopment -DryRun *> $null
        Require ($global:LakeReleaseFixture.Gh.Count -eq 0 -and $global:LakeReleaseFixture.Writes.Count -eq 0) 'Dry-run had side effects.'
    }
    Case 'Dirty source tree is rejected before publication' { param($p,$m); $global:LakeReleaseFixture.Dirty=$true; Reject $p }
    Case 'Package built from dirty source is rejected' { param($p,$m); $m.SourceDirty=$true; Save-Manifest $p $m; Reject $p }
    Case 'Package from another commit is rejected' { param($p,$m); $m.SourceCommit='b'*40; Save-Manifest $p $m; Reject $p }
    Case 'Unknown manifest fields cannot leak private metadata' {
        param($p,$m)
        $m | Add-Member -NotePropertyName PrivateFixturePath -NotePropertyValue 'synthetic-private-field'
        Save-Manifest $p $m;Reject $p
    }
    Case 'Arbitrary manifest notes cannot become a public release asset' {
        param($p,$m)
        $m.Notes += @('synthetic-private-notes');Save-Manifest $p $m;Reject $p
    }
    Case 'An altered asset hash is rejected' { param($p,$m); $m.Assets[0].Sha256='0'*64; Save-Manifest $p $m; Reject $p }
    Case 'Checksum contents must agree with the binary asset records' {
        param($p,$m)
        $path=Join-Path $p 'SHA256SUMS.txt'
        [IO.File]::WriteAllText($path,"not the expected checksums`r`n",[Text.Encoding]::ASCII)
        $m.Assets[2].Sha256=(Get-FileHash $path).Hash.ToLowerInvariant();$m.Assets[2].Bytes=(Get-Item $path).Length
        Save-Manifest $p $m;Reject $p
    }
    Case 'A ZIP containing an extra Contour.dll entry is rejected' {
        param($p,$m)
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $path=Join-Path $p $m.Assets[0].Name
        $zip=[IO.Compression.ZipFile]::Open($path,[IO.Compression.ZipArchiveMode]::Update)
        try { $entry=$zip.CreateEntry('Contour.dll');$stream=$entry.Open();try{$stream.WriteByte(0)}finally{$stream.Dispose()} }finally{$zip.Dispose()}
        $m.Assets[0].Sha256=(Get-FileHash $path).Hash.ToLowerInvariant();$m.Assets[0].Bytes=(Get-Item $path).Length
        $sums=($m.Assets[0..1] | ForEach-Object { "$($_.Sha256)  $($_.Name)" }) -join "`r`n"
        $sumPath=Join-Path $p 'SHA256SUMS.txt';[IO.File]::WriteAllText($sumPath,$sums+"`r`n",[Text.Encoding]::ASCII)
        $m.Assets[2].Sha256=(Get-FileHash $sumPath).Hash.ToLowerInvariant();$m.Assets[2].Bytes=(Get-Item $sumPath).Length
        Save-Manifest $p $m;Reject $p
    }
    Case 'A different origin/main commit is rejected' { param($p,$m);$global:LakeReleaseFixture.RemoteHead='b'*40;Reject $p }
    Case 'Publication requires current central product version and ABI metadata' {
        param($p,$m)
        $path=Join-Path $sandbox 'src\product_version.h'
        $original=[IO.File]::ReadAllText($path)
        try {
            [IO.File]::WriteAllText($path,$original.Replace('LAKE_FIX_ABI_VERSION '+$m.Provenance.Runtime.AbiVersion,'LAKE_FIX_ABI_VERSION 999'))
            Reject $p
        } finally { [IO.File]::WriteAllText($path,$original) }
    }
    Case 'Fetch/push repository mismatch is rejected' { param($p,$m);$global:LakeReleaseFixture.PushUrls=@('https://github.com/other/elsewhere.git');Reject $p }
    Case 'Any divergent secondary push URL is rejected' { param($p,$m);$global:LakeReleaseFixture.PushUrls+=@('git@github.com:other/elsewhere.git');Reject $p }
    Case 'Conflicting local tag is rejected even by dry-run' { param($p,$m);$global:LakeReleaseFixture.LocalTag='b'*40;Reject $p }
    Case 'Conflicting remote tag is never force-updated' { param($p,$m);$global:LakeReleaseFixture.RemoteTag='b'*40;Reject $p }
    Case 'A directory in place of the manifest is rejected' {
        param($p,$m)
        [IO.File]::Move((Join-Path $p 'RELEASE-MANIFEST.json'),(Join-Path $p 'manifest.saved'))
        New-Item -ItemType Directory -Path (Join-Path $p 'RELEASE-MANIFEST.json') | Out-Null
        Reject $p
    }
    Case 'Missing build provenance is rejected' {
        param($p,$m)
        $m.PSObject.Properties.Remove('Provenance');Save-Manifest $p $m;Reject $p
    }
    Case 'Missing or incompatible runtime test evidence is rejected' {
        param($p,$m)
        $m.Provenance.Runtime.Tests.Results=@();Save-Manifest $p $m;Reject $p
    }
    Case 'Missing setup test evidence is rejected' {
        param($p,$m)
        $m.Provenance.Setup.Tests.Results=@();Save-Manifest $p $m;Reject $p
    }
    Case 'Setup payload from another runtime build is rejected' {
        param($p,$m)
        $m.Provenance.Setup.Payload.RuntimeBuildId='f'*32
        $m.Provenance.Setup.PayloadManifestSha256=Get-LakeTextHash (($m.Provenance.Setup.Payload | ConvertTo-Json -Depth 24)+"`n")
        Save-Manifest $p $m;Reject $p
    }
    Case 'Unexpected nested provenance fields cannot leak private data' {
        param($p,$m)
        $m.Provenance.Runtime.Toolchain | Add-Member -NotePropertyName PrivateCompilerPath -NotePropertyValue 'synthetic-private-path'
        Save-Manifest $p $m;Reject $p
    }
    Case 'Duplicate JSON keys cannot hide private text discarded by parsing' {
        param($p,$m)
        $path=Join-Path $p 'RELEASE-MANIFEST.json'
        $text=[IO.File]::ReadAllText($path)
        [IO.File]::WriteAllText($path,'{"Notes":"synthetic-private-duplicate",'+$text.Substring(1),[Text.UTF8Encoding]::new($false))
        Reject $p
    }
    Case 'ZIP hashes are compared to tested payload even if asset checksums are recomputed' {
        param($p,$m)
        $path=Join-Path $p $m.Assets[0].Name
        $zip=[IO.Compression.ZipFile]::Open($path,[IO.Compression.ZipArchiveMode]::Update)
        try {
            $entry=$zip.GetEntry('LakeWindowedFix.ini');$entry.Delete()
            $stream=$zip.CreateEntry('LakeWindowedFix.ini').Open()
            try { $stream.WriteByte(42) } finally { $stream.Dispose() }
        } finally { $zip.Dispose() }
        $m.Assets[0].Sha256=(Get-FileHash $path).Hash.ToLowerInvariant();$m.Assets[0].Bytes=(Get-Item $path).Length
        $sums=($m.Assets[0..1] | ForEach-Object { "$($_.Sha256)  $($_.Name)" }) -join "`r`n"
        $sumPath=Join-Path $p 'SHA256SUMS.txt';[IO.File]::WriteAllText($sumPath,$sums+"`r`n",[Text.Encoding]::ASCII)
        $m.Assets[2].Sha256=(Get-FileHash $sumPath).Hash.ToLowerInvariant();$m.Assets[2].Bytes=(Get-Item $sumPath).Length
        Save-Manifest $p $m;Reject $p
    }
    Case 'Development unsigned publication is never an implicit default' {
        param($p,$m)
        $rejected=$false
        try { & "$sandbox\scripts\Publish-Release.ps1" -Version $version -PackageDirectory $p -GhPath $mockGh -DryRun *> $null }
        catch { $rejected=$true }
        Require ($rejected -and $global:LakeReleaseFixture.Gh.Count -eq 0 -and $global:LakeReleaseFixture.Writes.Count -eq 0) 'Unsigned opt-in was not enforced.'
    }
    Case 'Candidate check accepts dirty local source without consulting Git, tags or origin/main' {
        param($p,$m)
        $m.SourceDirty=$true;$m.Provenance.Runtime.SourceDirty=$true;$m.Provenance.Setup.SourceDirty=$true;$m.Provenance.Setup.Payload.SourceDirty=$true
        $m.Provenance.Setup.PayloadManifestSha256=Get-LakeTextHash (($m.Provenance.Setup.Payload | ConvertTo-Json -Depth 24)+"`n")
        Save-Manifest $p $m
        $global:LakeReleaseFixture.Dirty=$true;$global:LakeReleaseFixture.RemoteHead='b'*40
        & "$sandbox\scripts\Test-Candidate.ps1" -PackageDirectory $p -AllowUnsignedDevelopment *> $null
        Require ($global:LakeReleaseFixture.Git.Count -eq 0 -and $global:LakeReleaseFixture.Gh.Count -eq 0) 'Candidate tried Git/GitHub.'
        Reject $p
    }
    Case 'Mocked publish pins github.com and experimental flags, with exactly four assets' {
        param($p,$m)
        $global:LakeReleaseFixture.AllowWrites=$true
        & "$sandbox\scripts\Publish-Release.ps1" -Version $version -PackageDirectory $p -GhPath $mockGh -AllowUnsignedDevelopment *> $null
        $calls=$global:LakeReleaseFixture.Gh
        Require ($calls.Count -eq 3 -and $calls[0] -eq 'auth|status|--hostname|github.com') 'Wrong auth host or gh call count.'
        Require ($calls[1].Contains('--repo|github.com/example/lake-test')) 'GH_HOST could redirect the repository.'
        Require ($calls[2].Contains('--verify-tag') -and $calls[2].Contains('--prerelease') -and $calls[2].Contains('--latest=false')) 'Release safety flags absent.'
        $suffix=@($m.Assets.Name)+@('RELEASE-MANIFEST.json')
        foreach($name in $suffix){Require ($calls[2].Contains((Join-Path $p $name))) 'Expected asset absent.'}
        Require ($global:LakeReleaseFixture.Writes.Count -eq 2) 'Expected only mocked tag creation and tag push.'
    }
    Case 'Existing GitHub release is refused without modifying tags or assets' {
        param($p,$m)
        $global:LakeReleaseFixture.ExistingRelease=$true
        $rejected=$false
        try{& "$sandbox\scripts\Publish-Release.ps1" -Version $version -PackageDirectory $p -GhPath $mockGh -AllowUnsignedDevelopment *> $null}catch{$rejected=$true}
        Require ($rejected -and $global:LakeReleaseFixture.Writes.Count -eq 0 -and $global:LakeReleaseFixture.Gh.Count -eq 2) 'Existing release was not preserved.'
    }
} finally {
    if ($oldAlias) { Set-Alias -Name git -Value $oldAlias.Definition -Scope Global -Force }
    else { Remove-Item Alias:git -ErrorAction SilentlyContinue }
    Remove-Item -Path "Function:global:$functionName" -ErrorAction SilentlyContinue
    if ($oldState) { Set-Variable -Name LakeReleaseFixture -Scope Global -Value $oldState.Value }
    else { Remove-Variable LakeReleaseFixture -Scope Global -ErrorAction SilentlyContinue }
    $env:GH_HOST = $oldHost
}
Write-Host "Release package guards: $script:passed passed, $script:failed failed. No remote operations performed."
if ($script:failed) { throw 'Release package guard tests failed.' }
