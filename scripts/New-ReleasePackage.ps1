#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version = '0.6.0',
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$InstallerDirectory,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root "out\releases\v$Version" }
$output = [IO.Path]::GetFullPath($OutputDirectory)
$manifestPath = Join-Path $output 'RELEASE-MANIFEST.json'
$portableName = "LakeWindowedFix-v$Version-win32.zip"
$setupName = "LakeWindowedFix-Setup-v$Version.exe"
$allowedAssets = @($portableName,$setupName,'SHA256SUMS.txt','RELEASE-MANIFEST.json')
$runtimeNames = @('LakeWindowedLauncher.exe','LakeWindowedFix.dll','LakeWindowedFix.ini','LICENSE',
    'Measure-LakeResources.ps1','LakeResourceMetrics.psm1')
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$installer = Join-Path (Resolve-Path -LiteralPath $InstallerDirectory).Path 'LakeWindowedFix-Setup-v0.6.exe'
$projectVersion = [regex]::Match([IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')),
    'project\(lake_controller_windowed_fix VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
if ($projectVersion -ne $Version) { throw 'VERSION does not match the project.' }
if ((Get-FileHash (Join-Path $runtime 'LakeWindowedFix.ini')).Hash -ne (Get-FileHash (Join-Path $root 'config\LakeWindowedFix.ini')).Hash) {
    throw 'Refusing to package a locally edited INI.'
}
foreach ($name in $runtimeNames) {
    $item = Get-Item -LiteralPath (Join-Path $runtime $name)
    if ($item.PSIsContainer -or $item.Length -lt 1 -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsafe runtime file: $name" }
}
$setupFile = Get-Item -LiteralPath $installer
if ($setupFile.PSIsContainer -or ($setupFile.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe installer file.' }
if (Test-Path -LiteralPath $output) {
    $directory = Get-Item -LiteralPath $output
    if (!$directory.PSIsContainer -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe output directory.' }
}
$old = $null
foreach ($name in $allowedAssets) {
    $path = Join-Path $output $name
    if (Test-Path -LiteralPath $path) {
        $existing = Get-Item -LiteralPath $path
        if ($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsafe existing artifact preserved: $name" }
    }
}
if (Test-Path -LiteralPath $manifestPath) {
    $old = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ($old.Schema -ne 1 -or $old.Project -ne 'lake-controller-windowed-fix' -or $old.Version -ne $Version) { throw 'Unrecognized existing release manifest.' }
    $seen = @()
    foreach ($asset in $old.Assets) {
        if ($asset.Name -notin $allowedAssets -or $asset.Name -in $seen -or $asset.Sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Unsafe existing release manifest.' }
        $seen += $asset.Name
        $path = Join-Path $output $asset.Name
        if ((Test-Path -LiteralPath $path) -and (Get-FileHash -LiteralPath $path).Hash -ine $asset.Sha256) { throw "Modified release file preserved: $($asset.Name)" }
    }
}
foreach ($name in $allowedAssets) {
    if ((Test-Path -LiteralPath (Join-Path $output $name)) -and !$old) { throw "Unowned release file preserved: $name" }
    if ($old -and $name -ne 'RELEASE-MANIFEST.json' -and (Test-Path -LiteralPath (Join-Path $output $name)) -and $name -notin @($old.Assets.Name)) { throw "Unowned release file preserved: $name" }
}
$head = (& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[a-f0-9]{40}$') { throw 'Cannot determine source commit.' }
$dirty = @(& git -C $root status --porcelain --untracked-files=normal).Count -ne 0
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source status.' }
$stage = Join-Path $root ('private\release-staging\' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -ErrorAction Stop | Out-Null
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open((Join-Path $stage $portableName),[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($name in $runtimeNames) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $runtime $name),$name,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }
[IO.File]::Copy($installer,(Join-Path $stage $setupName),$false)
$records = foreach ($name in @($portableName,$setupName)) {
    $path = Join-Path $stage $name
    [pscustomobject]@{Name=$name;Sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();Bytes=(Get-Item -LiteralPath $path).Length}
}
$checksums = @($records | ForEach-Object { "$($_.Sha256)  $($_.Name)" }) -join "`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'),$checksums+"`r`n",[Text.Encoding]::ASCII)
$checksumPath = Join-Path $stage 'SHA256SUMS.txt'
$records += [pscustomobject]@{Name='SHA256SUMS.txt';Sha256=(Get-FileHash -LiteralPath $checksumPath).Hash.ToLowerInvariant();Bytes=(Get-Item -LiteralPath $checksumPath).Length}
$document = [ordered]@{
    Schema=1;Project='lake-controller-windowed-fix';Version=$Version;SourceCommit=$head;SourceDirty=$dirty
    RuntimeFiles=$runtimeNames;Assets=@($records);Experimental=$true
    Notes=@('MIT project files only; no proprietary Lake binaries or private evidence.',
        'Runtime tested OFFLINE only; no field/hardware approval or proven FPS/leak fix.',
        'Unsigned per-user installer; review Windows/EDR prompts according to local policy.')
}
[IO.File]::WriteAllText((Join-Path $stage 'RELEASE-MANIFEST.json'),($document | ConvertTo-Json -Depth 6)+"`n",[Text.UTF8Encoding]::new($false))
[IO.Directory]::CreateDirectory($output) | Out-Null
foreach ($name in $allowedAssets) {
    $source = Join-Path $stage $name
    $destination = Join-Path $output $name
    if (Test-Path -LiteralPath $destination) {
        # Only the previously verified owned artifact is replaced. Keep the old
        # copy privately; never recursively clean releases or unfamiliar files.
        [IO.File]::Replace($source,$destination,(Join-Path $stage ($name+'.previous')))
    } else { [IO.File]::Copy($source,$destination,$false) }
}
Write-Host "Release assets: $output"
Write-Host "Source: $head; dirty=$dirty. No GitHub release was created."
