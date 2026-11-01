#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Help','Build','Installer','Package')][string]$Target = 'Help',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version = '0.6.0',
    [AllowEmptyString()][string]$ZigPath = 'auto'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($Target -eq 'Help') {
    Write-Host @'
GNU Make / Windows - developer commands (end users only run the Setup EXE)
  make build          Build x86 runtime and run all runtime tests
  make test           Same tested build
  make installer      Build/test runtime and the native per-user Setup EXE
  make package        Produce installer, portable ZIP, checksums and manifest
  make release-check  Build/package and validate release, without publishing
  make release        Publish an experimental GitHub prerelease (explicit)
  make release-draft  Create a draft prerelease for review instead
Options: VERSION=0.6.0 ZIG=C:/tools/zig/zig.exe GH=gh.exe
Zig: existing complete portable toolchain or PATH; pinned to0.14.1, no download.
Publishing requires a clean committed/pushed tree and authenticated GitHub CLI.
'@
    return
}
$cmake = [IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'))
$match = [regex]::Match($cmake,'project\(lake_controller_windowed_fix VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (!$match.Success -or $match.Groups[1].Value -ne $Version) { throw 'VERSION must match the project version in CMakeLists.txt.' }
$runtime = Join-Path $root "dist-release-v$Version"
$setup = Join-Path $root "dist-setup-v$Version"
if ($Target -in @('Build','Installer')) {
    . (Join-Path $PSScriptRoot 'Resolve-Zig.ps1')
    $zig = Get-LakeZigPath -Requested $ZigPath
}
switch ($Target) {
    'Build' {
        & (Join-Path $PSScriptRoot 'Build.ps1') -Toolchain Zig -Configuration Release -ZigPath $zig -OutputDirectory $runtime
    }
    'Installer' {
        & (Join-Path $PSScriptRoot 'Build-Installer.ps1') -RuntimeDirectory $runtime -OutputDirectory $setup -ZigPath $zig
    }
    'Package' {
        & (Join-Path $PSScriptRoot 'New-ReleasePackage.ps1') -Version $Version -RuntimeDirectory $runtime -InstallerDirectory $setup
        & (Join-Path $root 'tests\Release-Package.Test.ps1') -PackageDirectory (Join-Path $root "out\releases\v$Version")
    }
}
