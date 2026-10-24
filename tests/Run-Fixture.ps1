[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryDirectory,
    [ValidateSet('0', '1')][string]$NativeResize = '0',
    [int]$ExpectedExit = -1,
    [switch]$ScaleRendering
)
$ErrorActionPreference = 'Stop'
if ($ExpectedExit -lt -1 -or $ExpectedExit -gt 255) { throw 'ExpectedExit invalido.' }
$source = (Resolve-Path -LiteralPath $BinaryDirectory).Path
# Fresh test-owned directory: never overwrite an INI or a DLL used by another
# fixture. All these files are generated, synthetic, and remain under build/.
$run = Join-Path (Join-Path $source 'test-runs') ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
foreach ($name in @('LakeWindowedFix.dll', 'LakeFixInjectionTest.exe', 'LakeFixFixture.exe', 'Contour.dll')) {
    [System.IO.File]::Copy((Join-Path $source $name), (Join-Path $run $name), $false)
}
$scaleValue = if ($ScaleRendering) { '1' } else { '0' }
$windowHeight = if ($ScaleRendering) { '576' } else { '768' }
# Legacy suites opt out explicitly now that scaling is the production default.
$ini = "[window]`r`nWidth=1024`r`nHeight=$windowHeight`r`n[compatibility]`r`nScaleRendering=$scaleValue`r`nNativeResize=$NativeResize`r`n"
if ($ScaleRendering) { $ini += "[render]`r`nWidth=1536`r`nHeight=864`r`n" }
[System.IO.File]::WriteAllText((Join-Path $run 'LakeWindowedFix.ini'), $ini, [System.Text.Encoding]::ASCII)
$arguments = @('--app-dir', $run)
if ($ExpectedExit -ge 0) { $arguments += @('--fixture-exit', "$ExpectedExit", '--wait') }
Write-Host "Synthetic fixture: ScaleRendering=$scaleValue NativeResize=$NativeResize ExpectedExit=$ExpectedExit"
& (Join-Path $run 'LakeFixInjectionTest.exe') @arguments
if ($LASTEXITCODE -ne 0) { throw "Fixture falhou (exit $LASTEXITCODE). Logs em $run\logs" }
Write-Host "Fixture reports: $run\logs"
