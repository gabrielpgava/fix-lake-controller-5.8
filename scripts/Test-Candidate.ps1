#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [Parameter(Mandatory)][string]$PackageDirectory,
    [switch]$AllowUnsignedDevelopment
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Build-Provenance.ps1')
$manifest = Test-LakeReleasePackage -PackageDirectory $PackageDirectory -Version $Version -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
Write-Host "Candidate approved: v$($manifest.Version); compiled source $($manifest.SourceCommit); dirty=$($manifest.SourceDirty)."
Write-Host 'Local integrity/test-evidence check only. No Git/gh, remote query, tag, upload or installation. Publication preflight is separate.'
