#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$RuntimeDirectory = 'dist-v0.6',
    [string]$OutputDirectory = 'dist-installer-v0.6',
    [string]$ZigPath = 'auto',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$runtime = if ([IO.Path]::IsPathRooted($RuntimeDirectory)) { $RuntimeDirectory } else { Join-Path $root $RuntimeDirectory }
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory } else { Join-Path $root $OutputDirectory }
$build = Join-Path $root 'build-zig-setup'
. (Join-Path $PSScriptRoot 'Resolve-Zig.ps1')
$zig = Get-LakeZigPath -Requested $ZigPath
New-Item -ItemType Directory -Path $build,$output -Force | Out-Null
if ((Get-FileHash -LiteralPath (Join-Path $runtime 'LakeWindowedFix.ini')).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $root 'config\LakeWindowedFix.ini')).Hash) {
    throw 'Use a clean runtime with the project default INI; do not distribute a local edited configuration.'
}
& (Join-Path $PSScriptRoot 'New-InstallerPayload.ps1') -RuntimeDirectory $runtime -OutputHeader (Join-Path $build 'installer_payload.h')
$common = @('c++','-target','x86-windows-gnu','-std=c++17','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0a00',
    '-Wall','-Wextra','-Wpedantic','-O2','-I',(Join-Path $root 'src'),'-I',$build)
$libraries = @('-lbcrypt','-luser32','-lgdi32','-lole32','-lshell32','-ladvapi32','-luuid')
& $zig cc -target x86-windows-gnu -c (Join-Path $root 'src\setup_manifest.S') -o (Join-Path $build 'setup_manifest.obj')
if ($LASTEXITCODE -ne 0) { throw 'Installer asInvoker manifest compilation failed.' }
if (!$SkipTests) {
    & $zig @common (Join-Path $root 'tests\setup_engine_test.cpp') (Join-Path $root 'src\setup_engine.cpp') (Join-Path $build 'setup_manifest.obj') @libraries -municode -o (Join-Path $build 'LakeFixSetupTest.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Installer test compilation failed.' }
    & (Join-Path $build 'LakeFixSetupTest.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Installer sandbox tests failed. Nothing was installed on this computer.' }
}
& $zig @common (Join-Path $root 'src\setup.cpp') (Join-Path $root 'src\setup_engine.cpp') (Join-Path $build 'setup_manifest.obj') @libraries -municode '-Wl,--subsystem,windows' -o (Join-Path $build 'LakeWindowedFix-Setup-v0.6.exe')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
Copy-Item -LiteralPath (Join-Path $build 'LakeWindowedFix-Setup-v0.6.exe') -Destination $output
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $output
Get-FileHash -LiteralPath (Join-Path $output 'LakeWindowedFix-Setup-v0.6.exe') -Algorithm SHA256 |
    ForEach-Object { "$($_.Hash.ToLowerInvariant())  LakeWindowedFix-Setup-v0.6.exe" } |
    Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ASCII
Write-Host "Installer ready: $output\LakeWindowedFix-Setup-v0.6.exe"
Write-Host 'Build/test only: no real installation or Lake launch was performed.'
