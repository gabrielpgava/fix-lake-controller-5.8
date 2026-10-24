[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryDirectory,
    [ValidateSet('0', '1')][string]$NativeResize = '0',
    [int]$ExpectedExit = -1
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
$ini = "[window]`r`nWidth=1024`r`nHeight=768`r`n[compatibility]`r`nNativeResize=$NativeResize`r`n"
[System.IO.File]::WriteAllText((Join-Path $run 'LakeWindowedFix.ini'), $ini, [System.Text.Encoding]::ASCII)
$arguments = @('--app-dir', $run)
if ($ExpectedExit -ge 0) { $arguments += @('--fixture-exit', "$ExpectedExit", '--wait') }
Write-Host "Synthetic fixture: NativeResize=$NativeResize ExpectedExit=$ExpectedExit"
& (Join-Path $run 'LakeFixInjectionTest.exe') @arguments
if ($LASTEXITCODE -ne 0) { throw "Fixture falhou (exit $LASTEXITCODE). Logs em $run\logs" }
Write-Host "Fixture reports: $run\logs"
