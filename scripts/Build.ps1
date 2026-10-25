[CmdletBinding()]
param(
    [ValidateSet('MSVC', 'MinGW', 'Zig')][string]$Toolchain = 'MSVC',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$ZigPath = 'zig.exe',
    [string]$MinGWCompiler = 'i686-w64-mingw32-g++.exe',
    [string]$OutputDirectory = 'dist',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build-$($Toolchain.ToLowerInvariant())"
$dist = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory } else { Join-Path $root $OutputDirectory }
$dist = [System.IO.Path]::GetFullPath($dist)
New-Item -ItemType Directory -Path $build, $dist -Force | Out-Null

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program falhou (exit $LASTEXITCODE)." }
}

if ($Toolchain -eq 'Zig') {
    # Optional portable toolchain. No download or global PATH/config changes.
    $zig = (Get-Command $ZigPath -ErrorAction Stop).Source
    $common = @('c++', '-target', 'x86-windows-gnu', '-std=c++17', '-DUNICODE', '-D_UNICODE',
                '-D_WIN32_WINNT=0x0a00', '-Wall', '-Wextra', '-Wpedantic')
    if ($Configuration -eq 'Release') { $common += '-O2' } else { $common += @('-O0', '-g') }
    Invoke-Checked $zig ($common + @('-shared', (Join-Path $root 'src/fix.cpp'), (Join-Path $root 'src/ddraw_scale.cpp'),
        (Join-Path $root 'src/fix.def'), '-luser32', '-lcomctl32', '-lgdi32', '-o', (Join-Path $build 'LakeWindowedFix.dll')))
    Invoke-Checked $zig ($common + @((Join-Path $root 'src/launcher.cpp'), '-lbcrypt', '-municode',
        '-o', (Join-Path $build 'LakeWindowedLauncher.exe')))
    if (!$SkipTests) {
        Invoke-Checked $zig ($common + @((Join-Path $root 'tests/scale_geometry_test.cpp'),
            '-municode', '-o', (Join-Path $build 'LakeFixScaleGeometryTest.exe')))
        Invoke-Checked $zig ($common + @((Join-Path $root 'tests/ui_quality_test.cpp'),
            '-municode', '-o', (Join-Path $build 'LakeFixUiQualityTest.exe')))
        Invoke-Checked $zig ($common + @((Join-Path $root 'tests/presenter_test.cpp'), (Join-Path $root 'src/ddraw_scale.cpp'),
            '-luser32', '-municode', '-o', (Join-Path $build 'LakeFixPresenterTest.exe')))
        Invoke-Checked (Join-Path $build 'LakeFixScaleGeometryTest.exe') @()
        Invoke-Checked (Join-Path $build 'LakeFixUiQualityTest.exe') @()
        Invoke-Checked (Join-Path $build 'LakeFixPresenterTest.exe') @()
        Invoke-Checked $zig ($common + @('-shared', (Join-Path $root 'tests/renderer_fixture.cpp'),
            '-luser32', '-lgdi32', '-o', (Join-Path $build 'Contour.dll')))
        Invoke-Checked $zig ($common + @((Join-Path $root 'tests/fixture.cpp'), '-luser32', '-lgdi32', '-municode',
            '-o', (Join-Path $build 'LakeFixFixture.exe')))
        Invoke-Checked $zig ($common + @((Join-Path $root 'src/launcher.cpp'), '-DLAKE_FIX_TESTING',
            '-lbcrypt', '-municode', '-o', (Join-Path $build 'LakeFixInjectionTest.exe')))
        & (Join-Path $root 'tests/Run-Fixture.ps1') -BinaryDirectory $build -NativeResize 0
        & (Join-Path $root 'tests/Run-Fixture.ps1') -BinaryDirectory $build -NativeResize 1
        & (Join-Path $root 'tests/Run-Fixture.ps1') -BinaryDirectory $build -NativeResize 0 -ExpectedExit 23
        & (Join-Path $root 'tests/Run-Fixture.ps1') -BinaryDirectory $build -NativeResize 0 -ScaleRendering
    }
    Copy-Item (Join-Path $build 'LakeWindowedFix.dll'), (Join-Path $build 'LakeWindowedLauncher.exe') $dist
} else {
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $testFlag = if ($SkipTests) { 'OFF' } else { 'ON' }
    $options = @('-S', $root, '-B', $build, "-DBUILD_TESTING=$testFlag")
    if ($Toolchain -eq 'MSVC') {
        $options += @('-G', 'Visual Studio 17 2022', '-A', 'Win32')
    } else {
        $compiler = (Get-Command $MinGWCompiler -ErrorAction Stop).Source
        $options += @('-G', 'Ninja', "-DCMAKE_CXX_COMPILER=$compiler", "-DCMAKE_BUILD_TYPE=$Configuration")
    }
    Invoke-Checked $cmake $options
    Invoke-Checked $cmake @('--build', $build, '--config', $Configuration, '--parallel')
    if (!$SkipTests) {
        $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
        Invoke-Checked $ctest @('--test-dir', $build, '-C', $Configuration, '--output-on-failure')
    }
    Invoke-Checked $cmake @('--install', $build, '--config', $Configuration, '--prefix', $dist)
}
# Configuration is user-editable: never overwrite it on an incremental build.
$ini = Join-Path $dist 'LakeWindowedFix.ini'
if (!(Test-Path $ini)) { Copy-Item (Join-Path $root 'config/LakeWindowedFix.ini') $ini }
Copy-Item (Join-Path $root 'LICENSE') $dist
Write-Host "Build x86 disponivel em $dist"
