#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('MSVC', 'MinGW', 'Zig')][string]$Toolchain = 'MSVC',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$ZigPath = 'zig.exe',
    [string]$MinGWCompiler = 'i686-w64-mingw32-g++.exe',
    [string]$OutputDirectory = 'dist',
    [switch]$SkipTests,
    [string]$BuildDirectory,
    [switch]$EnableProfiling,
    [string]$ObjcopyPath = 'auto'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Product-Metadata.ps1')
. (Join-Path $PSScriptRoot 'Build-Provenance.ps1')
. (Join-Path $PSScriptRoot 'Preserve-OwnSymbols.ps1')
$product = Get-LakeProductMetadata
if (!$BuildDirectory) { $BuildDirectory = "build-$($Toolchain.ToLowerInvariant())"; if ($EnableProfiling) { $BuildDirectory += '-profile' } }
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $root $BuildDirectory }
$build = Assert-LakeSafePath $build
if ($EnableProfiling -and !$PSBoundParameters.ContainsKey('OutputDirectory')) { $OutputDirectory = 'dist-profile' }
$dist = if ([IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory } else { Join-Path $root $OutputDirectory }
$dist = Assert-LakeSafePath $dist
New-Item -ItemType Directory -Path $build,$dist -Force | Out-Null

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    Add-LakeBuildCommand -Context $context -Program $Program -Arguments $Arguments
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)." }
}
function Copy-SupportFiles([string]$Directory) {
    foreach ($file in @('LICENSE','config\LakeWindowedFix.ini','scripts\Measure-LakeResources.ps1','scripts\LakeResourceMetrics.psm1')) {
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $Directory ([IO.Path]::GetFileName($file)))
    }
}

$compiler = if ($Toolchain -eq 'Zig') { (Get-Command $ZigPath -ErrorAction Stop).Source }
    elseif ($Toolchain -eq 'MSVC') { (Get-Command cl.exe -ErrorAction Stop).Source }
    else { (Get-Command $MinGWCompiler -ErrorAction Stop).Source }
$compilerVersion = if ($Toolchain -eq 'Zig') { (@(& $compiler version) -join '').Trim() }
    elseif ($Toolchain -eq 'MSVC') { (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion }
    else { (@(& $compiler -dumpfullversion) -join '').Trim() }
if ($Toolchain -eq 'Zig' -and $compilerVersion -ne '0.14.1') { throw 'Zig 0.14.1 is required.' }
$stamp = Join-Path $build 'generated\lake_build_stamp.h'
$context = Start-LakeBuildProvenance -RepositoryRoot $root -ManifestPath (Join-Path $build 'BUILD-PROVENANCE.json') `
    -Component Runtime -Version $product.Version -Toolchain $Toolchain -ToolchainVersion $compilerVersion -CompilerPath $compiler `
    -Configuration $Configuration -DistributionProfile DevelopmentUnsigned -EnableProfiling:$EnableProfiling `
    -GeneratedHeader $stamp -BuildArguments @($Toolchain,$Configuration,$BuildDirectory,$OutputDirectory,"Profiling=$EnableProfiling","SkipTests=$SkipTests","Objcopy=$ObjcopyPath")
$stage = Join-Path $build ('runtime-'+$context.Document.BuildId)
$symbols = Join-Path $build ('symbols\'+$context.Document.BuildId)
if (Test-Path -LiteralPath $stage) { throw 'Existing candidate preserved. Start a new build.' }
New-Item -ItemType Directory -Path $stage | Out-Null

if ($Toolchain -eq 'Zig') {
    $common = @('c++','-target','x86-windows-gnu','-std=c++17','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0a00',
        '-Wall','-Wextra','-Wpedantic','-include',$stamp,"-ffile-prefix-map=$root=.",'-g')
    if ($Configuration -eq 'Release') { $common += '-O2' } else { $common += '-O0' }
    $runtimeFlags = @()
    if ($EnableProfiling) { $runtimeFlags += @('-DLAKE_ENABLE_PROFILING=1','-fno-emulated-tls') }
    $runtimeSources = @((Join-Path $root 'src/fix.cpp'),(Join-Path $root 'src/ddraw_scale.cpp'),
        (Join-Path $root 'src/profile.cpp'),(Join-Path $root 'src/session_diagnostics.cpp'))
    $runtimeLibraries = @('-luser32','-lcomctl32','-lgdi32','-lbcrypt')
    Invoke-Checked $compiler ($common + $runtimeFlags + @('-shared') + $runtimeSources +
        @((Join-Path $root 'src/fix.def')) + $runtimeLibraries + @('-o',(Join-Path $build 'LakeWindowedFix.dll')))
    Invoke-Checked $compiler ($common + @((Join-Path $root 'src/launcher.cpp'),'-lbcrypt','-municode','-o',(Join-Path $build 'LakeWindowedLauncher.exe')))
    if (!$SkipTests) {
        $tests = @(
            @{Binary='LakeFixScaleGeometryTest';Sources=@('tests/scale_geometry_test.cpp')},
            @{Binary='LakeFixUiQualityTest';Sources=@('tests/ui_quality_test.cpp')},
            @{Binary='LakeFixPresenterTest';Sources=@('tests/presenter_test.cpp','src/ddraw_scale.cpp','src/profile.cpp');Flags=($runtimeFlags + @('-luser32'))},
            @{Binary='LakeFixProfileDisabledTest';Sources=@('tests/profile_test.cpp','src/profile.cpp')},
            @{Binary='LakeFixProfileEnabledTest';Sources=@('tests/profile_test.cpp','src/profile.cpp');Flags=@('-DLAKE_ENABLE_PROFILING=1','-fno-emulated-tls')},
            @{Binary='LakeFixStartupPolicyTest';Sources=@('tests/startup_policy_test.cpp');Flags=@('-DLAKE_FIX_TESTING','-lbcrypt')},
            @{Binary='LakeFixSessionConfigTest';Sources=@('tests/session_config_test.cpp');Flags=@('-lbcrypt')},
            @{Binary='LakeFixComponentContractTest';Sources=@('tests/component_contract_test.cpp')},
            @{Binary='LakeFixDiagnosticsTest';Sources=@('tests/diagnostics_test.cpp','src/session_diagnostics.cpp');Flags=@('-DLAKE_DIAGNOSTICS_TESTING','-lbcrypt')},
            @{Binary='LakeFixFixture';Sources=@('tests/fixture.cpp','tests/fixture.def');Flags=@('-luser32','-lgdi32')},
            @{Binary='LakeFixInjectionTest';Sources=@('src/launcher.cpp');Flags=@('-DLAKE_FIX_TESTING','-lbcrypt')}
        )
        foreach ($test in $tests) {
            $sources = @($test.Sources | ForEach-Object { Join-Path $root $_ })
            $testFlags = @($test.Flags | Where-Object { $null -ne $_ })
            Invoke-Checked $compiler ($common + $testFlags + $sources + @('-municode','-o',(Join-Path $build ($test.Binary+'.exe'))))
        }
        $startupSource = Join-Path $root 'tests/startup_policy_test.cpp'
        foreach ($child in @(@{Name='StartupChild';Flags=@()},@{Name='StartupEarlyChild';Flags=@('-DLAKE_STARTUP_ALWAYS_EARLY_EXIT')})) {
            Invoke-Checked $compiler ($common + @('-DLAKE_FIX_TESTING','-DLAKE_STARTUP_CHILD') + $child.Flags +
                @($startupSource,'-municode','-o',(Join-Path $build ($child.Name+'.exe'))))
        }
        $componentDef = Join-Path $build 'StartupComponent.def'
        $missingDef = Join-Path $build 'StartupComponentMissing.def'
        [IO.File]::WriteAllText($componentDef,"EXPORTS`n LakeWindowedQueryContract=LakeWindowedQueryContract@4`n LakeWindowedInitialize=LakeWindowedInitialize@4`n",[Text.Encoding]::ASCII)
        [IO.File]::WriteAllText($missingDef,"EXPORTS`n LakeWindowedInitialize=LakeWindowedInitialize@4`n",[Text.Encoding]::ASCII)
        foreach ($variant in @(@{Name='Good';Flags=@();Def=$componentDef},@{Name='Mismatch';Flags=@('-DLAKE_STARTUP_BAD_CONTRACT');Def=$componentDef},
            @{Name='Missing';Flags=@('-DLAKE_STARTUP_MISSING_QUERY');Def=$missingDef})) {
            Invoke-Checked $compiler ($common + @('-DLAKE_FIX_TESTING','-DLAKE_STARTUP_COMPONENT','-shared') + $variant.Flags +
                @($startupSource,$variant.Def,'-o',(Join-Path $build ('StartupComponent'+$variant.Name+'.dll'))))
        }
        Invoke-Checked $compiler ($common + @('-shared',(Join-Path $root 'tests/renderer_fixture.cpp'),'-luser32','-lgdi32','-o',(Join-Path $build 'Contour.dll')))
        Invoke-Checked $compiler ($common + $runtimeFlags + @('-DLAKE_FIX_TESTING','-shared') + $runtimeSources +
            @((Join-Path $root 'src/fix-test.def')) + $runtimeLibraries + @('-o',(Join-Path $build 'LakeWindowedFix.Test.dll')))
    }
    $binaryDirectory = $build
} else {
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $options = @('-S',$root,'-B',$build,"-DBUILD_TESTING=$(if ($SkipTests) {'OFF'} else {'ON'})",'-DLAKE_BUILD_SETUP=OFF',
        "-DLAKE_ENABLE_PROFILING=$(if ($EnableProfiling) {'ON'} else {'OFF'})","-DLAKE_BUILD_STAMP_HEADER=$stamp",
        "-DLAKE_TEST_PRODUCTION_DLL=$(Join-Path $stage 'LakeWindowedFix.dll')")
    if ($Toolchain -eq 'MSVC') { $options += @('-G','Visual Studio 17 2022','-A','Win32') }
    else { $options += @('-G','Ninja',"-DCMAKE_CXX_COMPILER=$compiler","-DCMAKE_BUILD_TYPE=$Configuration") }
    Invoke-Checked $cmake $options
    Invoke-Checked $cmake @('--build',$build,'--config',$Configuration,'--parallel')
    $binaryDirectory = if ($Toolchain -eq 'MSVC') { Join-Path $build $Configuration } else { $build }
}
foreach ($name in @('LakeWindowedFix.dll','LakeWindowedLauncher.exe')) {
    $null = Save-LakeOwnLinkSymbols -InputFile (Join-Path $binaryDirectory $name) -OutputFile (Join-Path $stage $name) `
        -SymbolDirectory $symbols -BuildId $context.Document.BuildId -Toolchain $Toolchain -ObjcopyPath $ObjcopyPath
}
Copy-SupportFiles $stage

if (!$SkipTests) {
    Start-LakeBuildTests -Context $context -ArtifactDirectory $stage
    Invoke-LakeBuildTest -Context $context -Name own_link_symbols -Program powershell.exe -Arguments @('-NoProfile','-File',
        (Join-Path $root 'tests/Link-Symbols.Test.ps1'),'-SymbolDirectory',$symbols,'-BinaryDirectory',$stage,'-BuildId',$context.Document.BuildId,'-ValidateOnly')
    if ($Toolchain -eq 'Zig') {
        $nativeTests = [ordered]@{scale_geometry='LakeFixScaleGeometryTest';ui_quality_policy='LakeFixUiQualityTest';
            scaled_presenter_objects='LakeFixPresenterTest';profiling_disabled='LakeFixProfileDisabledTest';profiling_enabled='LakeFixProfileEnabledTest';
            startup_policy='LakeFixStartupPolicyTest';session_configuration='LakeFixSessionConfigTest';diagnostics_queue='LakeFixDiagnosticsTest'}
        foreach ($name in $nativeTests.Keys) {
            Invoke-LakeBuildTest -Context $context -Name $name -Program (Join-Path $build ($nativeTests[$name]+'.exe'))
        }
        Invoke-LakeBuildTest -Context $context -Name component_contract -Program (Join-Path $build 'LakeFixComponentContractTest.exe') -Arguments @((Join-Path $stage 'LakeWindowedFix.dll'))
        foreach ($test in @(@('resource_metrics_policy','Resource-Metrics.Test.ps1'),@('isolated_build_options','Build-Options.Test.ps1'),@('monitor_lifecycle_policy','Monitor-Policy.Test.ps1'))) {
            Invoke-LakeBuildTest -Context $context -Name $test[0] -Program powershell.exe -Arguments @('-NoProfile','-File',(Join-Path $root ('tests\'+$test[1])))
        }
        $python = (@(& python -c 'import sys; print(sys.executable)') -join '').Trim()
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $python -PathType Leaf)) { throw 'Python is required for read-only window snapshot tests.' }
        Invoke-LakeBuildTest -Context $context -Name window_state_snapshots -Program $python -Arguments @((Join-Path $root 'tests/window_state_test.py'))
        $profilingValue = if ($EnableProfiling) { '1' } else { '0' }
        foreach ($case in @(
            @{Name='windows_safe_no_rebuild';Args=@('-NativeResize','0')},
            @{Name='windows_experimental_resize';Args=@('-NativeResize','1')},
            @{Name='windows_exit_observer';Args=@('-NativeResize','0','-ExpectedExit','23')},
            @{Name='windows_scaled_window';Args=@('-NativeResize','0','-ScaleRendering')}
        )) {
            Invoke-LakeBuildTest -Context $context -Name $case.Name -Program powershell.exe -Arguments (@('-NoProfile','-File',
                (Join-Path $root 'tests/Run-Fixture.ps1'),'-BinaryDirectory',$build,'-Profiling',$profilingValue) + $case.Args)
        }
    } else {
        $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
        foreach ($name in @(Get-LakeRequiredTests Runtime | Where-Object { $_ -cne 'own_link_symbols' })) {
            Invoke-LakeBuildTest -Context $context -Name $name -Program $ctest -Arguments @('--test-dir',$build,'-C',$Configuration,'--output-on-failure','--no-tests=error','-R',"^$name`$")
        }
    }
}
foreach ($name in @('LakeWindowedFix.dll','LakeWindowedLauncher.exe')) {
    $null = Test-LakeOwnLinkSymbolRecord -RecordFile (Join-Path $symbols ($name+'.link-symbols.json')) -OutputFile (Join-Path $stage $name) -BuildId $context.Document.BuildId
}
$document = Complete-LakeBuildProvenance -Context $context -ArtifactDirectory $stage -RequireTests:(!$SkipTests)
Copy-Item (Join-Path $build 'BUILD-PROVENANCE.json') $symbols

foreach ($name in @(Get-LakeRuntimeNames)) {
    $destination = Join-Path $dist $name
    if ($name -eq 'LakeWindowedFix.ini' -and (Test-Path -LiteralPath $destination)) { continue }
    Copy-Item -LiteralPath (Join-Path $stage $name) -Destination $destination
}
if ((Get-FileHash -LiteralPath (Join-Path $dist 'LakeWindowedFix.ini')).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $stage 'LakeWindowedFix.ini')).Hash) {
    # Incremental developer builds preserve edited configuration, but cannot
    # pretend that it is the clean runtime attested for distribution.
    $document.State = 'Building'
    Write-LakeJson (Join-Path $dist 'BUILD-PROVENANCE.json') $document
    Write-Host 'Edited INI preserved. This local dist is not a package candidate.'
} else {
    Copy-Item (Join-Path $build 'BUILD-PROVENANCE.json') $dist
}
Write-Host "Build x86 available in $dist; build=$($document.BuildId); unsigned development."
if ($EnableProfiling) { Write-Host 'DIAGNOSTIC BUILD: profiling enabled; no Lake launch, installer or performance claim.' }
