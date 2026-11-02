#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Help','Build','Profile','Installer','Package')][string]$Target = 'Help',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [AllowEmptyString()][string]$ZigPath = 'auto',
    [string]$ObjcopyPath = 'auto'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Product-Metadata.ps1')
$product = Get-LakeProductMetadata
if (!$Version) { $Version = $product.Version }
if ($Target -eq 'Help') {
    Write-Host @'
GNU Make / Windows - developer commands (end users only run the Setup EXE)
  make build          Build x86 runtime and run all runtime tests
  make test           Same tested build
  make profile        Build/test a SEPARATE diagnostic runtime; no Lake launch
  make installer      Build/test runtime and the native per-user Setup EXE
  make package        Produce installer, portable ZIP, checksums and manifest
  make release-check  Build/package and validate candidate on any work branch
  make publish-check  Separate read-only publication preflight (origin/main gate)
  make graphics-check-build  Build own diagnostic; NEVER opens desktop by itself
  make release        Publish an experimental GitHub prerelease (explicit)
  make release-draft  Create a draft prerelease for review instead
Options: VERSION=<central product version> ZIG=C:/tools/zig/zig.exe OBJCOPY=C:/tools/LLVM/bin/llvm-objcopy.exe GH=gh.exe
Zig: existing complete portable toolchain or PATH; pinned to 0.14.1, no download.
LLVM objcopy: existing native tool for own DWARF symbols; not a compiler replacement.
Publishing requires a clean committed/pushed tree and authenticated GitHub CLI.
'@
    return
}
if ($Version -ne $product.Version) { throw 'VERSION must match src/product_version.h.' }
$runtime = Join-Path $root "dist-release-v$Version"
$setup = Join-Path $root "dist-setup-v$Version"
if ($Target -in @('Build','Profile','Installer')) {
    . (Join-Path $PSScriptRoot 'Resolve-Zig.ps1')
    $zig = Get-LakeZigPath -Requested $ZigPath
}
switch ($Target) {
    'Build' {
        & (Join-Path $PSScriptRoot 'Build.ps1') -Toolchain Zig -Configuration Release -ZigPath $zig -OutputDirectory $runtime -ObjcopyPath $ObjcopyPath
    }
    'Profile' {
        & (Join-Path $PSScriptRoot 'Build.ps1') -Toolchain Zig -Configuration Release -ZigPath $zig -EnableProfiling -OutputDirectory (Join-Path $root "dist-profile-v$Version") -ObjcopyPath $ObjcopyPath
    }
    'Installer' {
        & (Join-Path $PSScriptRoot 'Build-Installer.ps1') -RuntimeDirectory $runtime -OutputDirectory $setup -ZigPath $zig -AllowUnsignedDevelopment -ObjcopyPath $ObjcopyPath
    }
    'Package' {
        & (Join-Path $PSScriptRoot 'New-ReleasePackage.ps1') -Version $Version -RuntimeDirectory $runtime -InstallerDirectory $setup -AllowUnsignedDevelopment
        & (Join-Path $root 'tests\Release-Package.Test.ps1') -PackageDirectory (Join-Path $root "out\releases\v$Version")
    }
}
