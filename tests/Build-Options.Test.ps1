#Requires -Version 5.1
# The compiler is a PowerShell mock. Fake EXE/DLL files are plain text, are
# written only into a fresh private sandbox and are NEVER executed or loaded.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sandbox = Join-Path $root ('private\build-option-tests\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sandbox | Out-Null
$mock = Join-Path $sandbox 'mock-zig.ps1'
[IO.File]::WriteAllText($mock,@'
$ErrorActionPreference = 'Stop'
$arguments = @($args)
if ($arguments.Count -eq 1 -and $arguments[0] -eq 'version') { Write-Output '0.14.1'; $global:LASTEXITCODE = 0; return }
$index = [Array]::IndexOf($arguments,'-o')
if ($index -lt 0 -or $index+1 -ge $arguments.Count) { throw 'Unexpected mock compiler arguments.' }
$output = [IO.Path]::GetFullPath($arguments[$index+1])
$allowed = $PSScriptRoot.TrimEnd('\')+'\'
if (!$output.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) { throw 'Mock output escaped its sandbox.' }
if (Test-Path -LiteralPath $output) {
    if ([IO.File]::ReadAllText($output) -cne 'synthetic compiler output; not a PE binary') { throw 'Foreign fixture file preserved.' }
}
[IO.File]::WriteAllText($output,'synthetic compiler output; not a PE binary',[Text.Encoding]::ASCII)
Add-Content -LiteralPath $env:LAKE_MOCK_COMPILER_LOG -Value (@{Arguments=$arguments} | ConvertTo-Json -Compress) -Encoding UTF8
$global:LASTEXITCODE = 0
'@,[Text.Encoding]::ASCII)
$previousLog = $env:LAKE_MOCK_COMPILER_LOG
$previousExit = Get-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
$symbolAliases = @{}
foreach ($name in @('Save-LakeOwnLinkSymbols','Test-LakeOwnLinkSymbolRecord')) { $symbolAliases[$name] = Get-Alias $name -Scope Global -ErrorAction SilentlyContinue }
$symbolMock = 'Invoke-LakeBuildSymbolMock-'+[guid]::NewGuid().ToString('N')
# Alias precedence substitutes ONLY the symbol I/O layer in this test process.
# The real build has no skip-symbols switch or production bypass. These are
# deliberately non-PE mock outputs and are never passed to objcopy/LoadLibrary.
Set-Item -Path ('Function:global:'+$symbolMock) -Value {
    param($InputFile,$OutputFile,$SymbolDirectory,$BuildId,$Toolchain,$ObjcopyPath,$RecordFile)
    $parent = Split-Path $env:LAKE_MOCK_COMPILER_LOG -Parent
    foreach ($path in @($InputFile,$OutputFile,$SymbolDirectory,$RecordFile) | Where-Object { $_ }) {
        if (![IO.Path]::GetFullPath($path).StartsWith($parent+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Mock symbols escaped their sandbox.' }
    }
    if ($BuildId -cnotmatch '^[a-f0-9]{32}$') { throw 'Mock symbols did not receive the exact build ID.' }
    if ($InputFile) {
        if ([IO.File]::ReadAllText($InputFile) -cne 'synthetic compiler output; not a PE binary' -or (Test-Path -LiteralPath $OutputFile)) { throw 'Mock symbol input/output identity error.' }
        [IO.Directory]::CreateDirectory($SymbolDirectory) | Out-Null
        [IO.File]::Copy($InputFile,$OutputFile,$false)
        Add-Content -LiteralPath ($env:LAKE_MOCK_COMPILER_LOG+'.symbols') -Value $BuildId
    } elseif ([IO.File]::ReadAllText($OutputFile) -cne 'synthetic compiler output; not a PE binary') { throw 'Mock final output changed.' }
}
foreach ($name in $symbolAliases.Keys) { Set-Alias -Name $name -Value $symbolMock -Scope Global -Force }
$script:checks = 0
function Check([bool]$Condition,[string]$Label) {
    if (!$Condition) { throw "FAIL: $Label" }
    $script:checks++
    Write-Host "PASS: $Label"
}
try {
    foreach ($enabled in @($false,$true)) {
        $label = if ($enabled) { 'profile' } else { 'normal' }
        $build = Join-Path $sandbox "$label-build"
        $dist = Join-Path $sandbox "$label-dist"
        $env:LAKE_MOCK_COMPILER_LOG = Join-Path $sandbox "$label-compiler.jsonl"
        & (Join-Path $root 'scripts\Build.ps1') -Toolchain Zig -ZigPath $mock -BuildDirectory $build -OutputDirectory $dist -EnableProfiling:$enabled -SkipTests
        $calls = @(Get-Content -LiteralPath $env:LAKE_MOCK_COMPILER_LOG | ForEach-Object { $_ | ConvertFrom-Json })
        Check ($calls.Count -eq 2) "$label build invokes only the two mocked compiles when tests are skipped"
        $manifest = [IO.File]::ReadAllText((Join-Path $build 'BUILD-PROVENANCE.json')) | ConvertFrom-Json
        Check (@(Get-Content -LiteralPath ($env:LAKE_MOCK_COMPILER_LOG+'.symbols') | Where-Object { $_ -ceq $manifest.BuildId }).Count -eq 2 -and
            $manifest.Tests.Results.Count -eq 0 -and $manifest.Arguments.Count -ge 9) "$label build separates both final images with the same build ID and captures actual commands without claiming tests"
        $runtime = @($calls | Where-Object { $_.Arguments -contains (Join-Path $root 'src/fix.cpp') })
        $launcher = @($calls | Where-Object { $_.Arguments -contains (Join-Path $root 'src/launcher.cpp') })
        Check ($runtime.Count -eq 1 -and $launcher.Count -eq 1 -and
            (($runtime[0].Arguments -contains '-DLAKE_ENABLE_PROFILING=1') -eq $enabled) -and
            (($runtime[0].Arguments -contains '-fno-emulated-tls') -eq $enabled) -and
            ($launcher[0].Arguments -notcontains '-DLAKE_ENABLE_PROFILING=1') -and
            ($launcher[0].Arguments -notcontains '-fno-emulated-tls')) "$label build selects profiling and its native TLS guard only on the runtime"
        Check ((Get-Item -LiteralPath (Join-Path $dist 'LakeWindowedFix.dll')).Length -gt 0 -and
            (Get-FileHash -LiteralPath (Join-Path $dist 'LakeWindowedFix.ini')).Hash -ceq
            (Get-FileHash -LiteralPath (Join-Path $root 'config\LakeWindowedFix.ini')).Hash) "$label output is isolated and starts with the unchanged default INI"
    }
    $editedIni = Join-Path $sandbox 'normal-dist\LakeWindowedFix.ini'
    [IO.File]::WriteAllText($editedIni,"[synthetic-user]`r`nPreserve=1`r`n",[Text.Encoding]::ASCII)
    $before = (Get-FileHash -LiteralPath $editedIni).Hash
    $env:LAKE_MOCK_COMPILER_LOG = Join-Path $sandbox 'incremental-compiler.jsonl'
    & (Join-Path $root 'scripts\Build.ps1') -Toolchain Zig -ZigPath $mock -BuildDirectory (Join-Path $sandbox 'normal-build') -OutputDirectory (Join-Path $sandbox 'normal-dist') -SkipTests
    Check ((Get-FileHash -LiteralPath $editedIni).Hash -ceq $before) 'isolated incremental build preserves an edited INI'
} finally {
    foreach ($name in $symbolAliases.Keys) {
        if ($symbolAliases[$name]) { Set-Alias -Name $name -Value $symbolAliases[$name].Definition -Scope Global -Force }
        else { Remove-Item -LiteralPath ('Alias:global:'+$name) -ErrorAction SilentlyContinue }
    }
    Remove-Item -LiteralPath ('Function:global:'+$symbolMock) -ErrorAction SilentlyContinue
    $env:LAKE_MOCK_COMPILER_LOG = $previousLog
    if ($previousExit) { Set-Variable LASTEXITCODE -Scope Global -Value $previousExit.Value }
    else { Remove-Variable LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue }
}
Write-Host "Build options: $script:checks passed; no native compiler or generated binary was executed."
