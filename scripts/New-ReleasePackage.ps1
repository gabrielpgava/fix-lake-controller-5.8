#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$InstallerDirectory,
    [string]$OutputDirectory,
    [switch]$AllowUnsignedDevelopment
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Build-Provenance.ps1')
$root = Split-Path $PSScriptRoot -Parent
$runtime = Assert-LakeSafePath (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$installer = Assert-LakeSafePath (Resolve-Path -LiteralPath $InstallerDirectory).Path
$runtimePath = Join-Path $runtime 'BUILD-PROVENANCE.json'
$setupPath = Join-Path $installer 'BUILD-PROVENANCE.json'
$runtimeBuild = Read-LakeJson $runtimePath
$setupBuild = Read-LakeJson $setupPath
Assert-LakeProvenancePair $runtimeBuild $setupBuild -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
if (!$Version) { $Version = $runtimeBuild.Version }
if ($Version -cne $runtimeBuild.Version) { throw 'VERSION does not match the compiled artifacts.' }
$runtimeHash = (Get-LakeFileRecord $runtimePath 'runtime-manifest').Sha256
$setupHash = (Get-LakeFileRecord $setupPath 'setup-manifest').Sha256
if ($setupBuild.RuntimeManifestSha256 -cne $runtimeHash) { throw 'Setup was compiled with another runtime manifest.' }
Assert-LakePayloadMatchesRuntime $setupBuild.Payload $runtimeBuild $runtimeHash
$runtimeNames = @(Get-LakeRuntimeNames)
Assert-LakeSameRecords @(Get-LakeArtifactRecords $runtime $runtimeNames) $runtimeBuild.Files 'runtime build'
Assert-LakeSameRecords @(Get-LakeArtifactRecords $installer @($setupBuild.Files[0].Name)) $setupBuild.Files 'setup build'
foreach ($name in @('LakeWindowedLauncher.exe','LakeWindowedFix.dll')) { Assert-LakeX86Pe (Join-Path $runtime $name) }
$setupSource = Join-Path $installer $setupBuild.Files[0].Name
Assert-LakeX86Pe $setupSource

# Intentionally no Git or current CMake/default INI reads: an older compiled
# dist retains its original commit/version/defaults, even after HEAD changes.
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root "out\releases\v$Version" }
$output = Assert-LakeSafePath $OutputDirectory
$parent = Split-Path $output -Parent
foreach ($input in @($runtime,$installer)) {
    if ($output -ieq $input -or $output.StartsWith($input.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
        $input.StartsWith($output.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Release output must not overlap build inputs.' }
}
if (Test-Path -LiteralPath $output) {
    $null = Test-LakeReleasePackage $output -Version $Version -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
}
[IO.Directory]::CreateDirectory($parent) | Out-Null
$stage = Join-Path $parent ('.lake-stage-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($stage) | Out-Null
$portableName = "LakeWindowedFix-v$Version-win32.zip"
$setupName = "LakeWindowedFix-Setup-v$Version.exe"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open((Join-Path $stage $portableName),[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($name in $runtimeNames) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $runtime $name),$name,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }
[IO.File]::Copy($setupSource,(Join-Path $stage $setupName),$false)
$records = @(Get-LakeArtifactRecords $stage @($portableName,$setupName))
$checksums = (@($records | ForEach-Object { "$($_.Sha256)  $($_.Name)" }) -join "`r`n")+"`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'),$checksums,[Text.Encoding]::ASCII)
$records += Get-LakeFileRecord (Join-Path $stage 'SHA256SUMS.txt') 'SHA256SUMS.txt'
$document = [pscustomobject][ordered]@{
    Schema=2;Project='lake-controller-windowed-fix';Version=$Version
    SourceCommit=$runtimeBuild.SourceCommit;SourceDirty=$runtimeBuild.SourceDirty
    RuntimeFiles=$runtimeNames;Assets=$records;Experimental=$true;Notes=@(Get-LakeReleaseNotes)
    Provenance=[pscustomobject]@{
        Runtime=(ConvertTo-LakePublicProvenance $runtimeBuild $runtimeHash)
        Setup=(ConvertTo-LakePublicProvenance $setupBuild $setupHash)
    }
}
Write-LakeJson (Join-Path $stage 'RELEASE-MANIFEST.json') $document
# Re-read staged ZIP contents and setup hashes before a single directory move.
# Failure leaves a private sibling stage, never a partially replaced release.
$null = Test-LakeReleasePackage $stage -Version $Version -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
Publish-LakeReleaseDirectory -Stage $stage -Destination $output -Version $Version -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
Write-Host "Release assets: $output"
Write-Host "Compiled source: $($runtimeBuild.SourceCommit); dirty=$($runtimeBuild.SourceDirty). No GitHub release was created."
