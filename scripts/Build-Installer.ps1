#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$RuntimeDirectory,
    [string]$OutputDirectory,
    [ValidateSet('Zig','MSVC','MinGW')][string]$Toolchain = 'Zig',
    [string]$ZigPath = 'auto',
    [string]$MinGWCompiler = 'i686-w64-mingw32-g++.exe',
    [string]$BuildDirectory,
    [switch]$SkipTests,
    [switch]$AllowUnsignedDevelopment,
    [string]$ObjcopyPath = 'auto'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Product-Metadata.ps1')
. (Join-Path $PSScriptRoot 'Build-Provenance.ps1')
. (Join-Path $PSScriptRoot 'Preserve-OwnSymbols.ps1')
$product = Get-LakeProductMetadata
if (!$RuntimeDirectory) { $RuntimeDirectory = "dist-release-v$($product.Version)" }
if (!$OutputDirectory) { $OutputDirectory = "dist-setup-v$($product.Version)" }
if (!$AllowUnsignedDevelopment) { throw 'No certificate/signing profile configured. Explicitly select -AllowUnsignedDevelopment for a development candidate.' }
$runtime = Assert-LakeSafePath (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$output = Assert-LakeSafePath $(if ([IO.Path]::IsPathRooted($OutputDirectory)) {$OutputDirectory} else {Join-Path $root $OutputDirectory})
if (!$BuildDirectory) { $BuildDirectory = "build-$($Toolchain.ToLowerInvariant())-setup" }
$build = Assert-LakeSafePath $(if ([IO.Path]::IsPathRooted($BuildDirectory)) {$BuildDirectory} else {Join-Path $root $BuildDirectory})
New-Item -ItemType Directory -Path $build,$output -Force | Out-Null
$payload = Join-Path $build 'installer_payload.h'
& (Join-Path $PSScriptRoot 'New-InstallerPayload.ps1') -RuntimeDirectory $runtime -OutputHeader $payload -AllowUnsignedDevelopment
if ($Toolchain -eq 'Zig') {
    . (Join-Path $PSScriptRoot 'Resolve-Zig.ps1')
    $compiler = Get-LakeZigPath -Requested $ZigPath
    $compilerVersion = (@(& $compiler version) -join '').Trim()
} elseif ($Toolchain -eq 'MSVC') {
    $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    $compilerVersion = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
} else {
    $compiler = (Get-Command $MinGWCompiler -ErrorAction Stop).Source
    $compilerVersion = (@(& $compiler -dumpfullversion) -join '').Trim()
}
$stamp = Join-Path $build 'generated\lake_setup_stamp.h'
$context = Start-LakeBuildProvenance -RepositoryRoot $root -ManifestPath (Join-Path $build 'BUILD-PROVENANCE.json') `
    -Component Setup -Version $product.Version -Toolchain $Toolchain -ToolchainVersion $compilerVersion -CompilerPath $compiler `
    -Configuration Release -DistributionProfile DevelopmentUnsigned -RuntimeManifestPath (Join-Path $runtime 'BUILD-PROVENANCE.json') `
    -PayloadManifestPath ($payload+'.manifest.json') -PayloadHeaderPath $payload -InstallerFileName $product.SetupName `
    -GeneratedHeader $stamp -BuildArguments @($Toolchain,$RuntimeDirectory,$OutputDirectory,"SkipTests=$SkipTests","Objcopy=$ObjcopyPath")
$stage = Join-Path $build ('setup-'+$context.Document.BuildId)
$symbols = Join-Path $build ('symbols\'+$context.Document.BuildId)
if (Test-Path -LiteralPath $stage) { throw 'Existing setup candidate preserved.' }
New-Item -ItemType Directory -Path $stage | Out-Null
function Invoke-Checked([string]$Program,[string[]]$Arguments) {
    Add-LakeBuildCommand -Context $context -Program $Program -Arguments $Arguments
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)." }
}
if ($Toolchain -eq 'Zig') {
    $common = @('c++','-target','x86-windows-gnu','-std=c++17','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0a00',
        '-Wall','-Wextra','-Wpedantic','-O2','-g',"-ffile-prefix-map=$root=.",'-include',$stamp,'-I',(Join-Path $root 'src'),'-I',$build)
    $libraries = @('-lbcrypt','-luser32','-lgdi32','-lole32','-lshell32','-ladvapi32','-luuid')
    $manifest = Join-Path $build 'setup_manifest.obj'
    Invoke-Checked $compiler @('cc','-target','x86-windows-gnu','-c',(Join-Path $root 'src/setup_manifest.S'),'-o',$manifest)
    Invoke-Checked $compiler ($common + @((Join-Path $root 'src/setup.cpp'),(Join-Path $root 'src/setup_engine.cpp'),$manifest) +
        $libraries + @('-municode','-Wl,--subsystem,windows','-o',(Join-Path $build $product.SetupName)))
    $binaryDirectory = $build
    if (!$SkipTests) {
        Invoke-Checked $compiler ($common + @((Join-Path $root 'tests/setup_engine_test.cpp'),(Join-Path $root 'src/setup_engine.cpp'),$manifest) +
            $libraries + @('-municode','-o',(Join-Path $build 'LakeFixSetupTest.exe')))
        $sandboxTest = Join-Path $build 'LakeFixSetupTest.exe'
    }
} else {
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $options = @('-S',$root,'-B',$build,'-DBUILD_TESTING=ON','-DLAKE_BUILD_SETUP=ON','-DLAKE_ENABLE_PROFILING=OFF',
        "-DLAKE_RUNTIME_DIRECTORY=$runtime","-DLAKE_SETUP_PAYLOAD_HEADER=$payload","-DLAKE_BUILD_STAMP_HEADER=$stamp")
    if ($Toolchain -eq 'MSVC') { $options += @('-G','Visual Studio 17 2022','-A','Win32') }
    else { $options += @('-G','Ninja',"-DCMAKE_CXX_COMPILER=$compiler",'-DCMAKE_BUILD_TYPE=Release') }
    Invoke-Checked $cmake $options
    Invoke-Checked $cmake @('--build',$build,'--config','Release','--target','LakeWindowedSetup','LakeFixSetupTest','--parallel')
    $binaryDirectory = if ($Toolchain -eq 'MSVC') { Join-Path $build 'Release' } else { $build }
    $sandboxTest = Join-Path $binaryDirectory 'LakeFixSetupTest.exe'
}
$null = Save-LakeOwnLinkSymbols -InputFile (Join-Path $binaryDirectory $product.SetupName) -OutputFile (Join-Path $stage $product.SetupName) `
    -SymbolDirectory $symbols -BuildId $context.Document.BuildId -Toolchain $Toolchain -ObjcopyPath $ObjcopyPath
if (!$SkipTests) {
    Start-LakeBuildTests -Context $context -ArtifactDirectory $stage
    Invoke-LakeBuildTest -Context $context -Name installer_sandbox -Program $sandboxTest
    Invoke-LakeBuildTest -Context $context -Name installer_payload_integrity -Program (Join-Path $stage $product.SetupName) -Arguments @('--verify-payload')
}
$null = Test-LakeOwnLinkSymbolRecord -RecordFile (Join-Path $symbols ($product.SetupName+'.link-symbols.json')) `
    -OutputFile (Join-Path $stage $product.SetupName) -BuildId $context.Document.BuildId
$document = Complete-LakeBuildProvenance -Context $context -ArtifactDirectory $stage -RequireTests:(!$SkipTests)
Copy-Item -LiteralPath (Join-Path $build 'BUILD-PROVENANCE.json') -Destination $symbols
Copy-Item -LiteralPath (Join-Path $stage $product.SetupName) -Destination $output
Copy-Item -LiteralPath (Join-Path $build 'BUILD-PROVENANCE.json') -Destination $output
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $output
Get-FileHash -LiteralPath (Join-Path $output $product.SetupName) -Algorithm SHA256 |
    ForEach-Object { "$($_.Hash.ToLowerInvariant())  $($product.SetupName)" } |
    Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ASCII
Write-Host "Setup ready: $output\$($product.SetupName); build=$($document.BuildId); UNSIGNED DEVELOPMENT."
Write-Host 'No operational installation or Lake launch was performed.'
