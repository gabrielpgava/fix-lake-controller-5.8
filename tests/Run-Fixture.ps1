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
$scaleValue = if ($ScaleRendering) { '1' } else { '0' }
$windowHeight = if ($ScaleRendering) { '576' } else { '768' }
$cases = @(@{ Name = 'legacy'; Width = 1024; Height = $windowHeight; Minimum = "MinWidth=2048`r`nMinHeight=1536`r`n"; PolicyOnly = $false })
if ($ScaleRendering -and $NativeResize -eq '0' -and $ExpectedExit -lt 0) {
    # Keep the complete input/400x300 viewport coverage with an explicit opt-out.
    # Separate hidden-window runs cover production defaults and larger minima.
    $cases = @(
        @{ Name = 'scaled-input'; Width = 1024; Height = 576; Minimum = "MinWidth=320`r`nMinHeight=180`r`n"; PolicyOnly = $false },
        @{ Name = 'scaled-default-minimum'; Width = 1024; Height = 576; Minimum = ''; PolicyOnly = $true },
        @{ Name = 'scaled-custom-minimum'; Width = 320; Height = 180; Minimum = "MinWidth=1200`r`nMinHeight=675`r`n"; PolicyOnly = $true },
        @{ Name = 'scaled-invalid-width'; Width = 320; Height = 180; Minimum = "MinWidth=8193`r`nMinHeight=620`r`n"; PolicyOnly = $true },
        @{ Name = 'scaled-invalid-height'; Width = 320; Height = 180; Minimum = "MinWidth=960`r`nMinHeight=179`r`n"; PolicyOnly = $true },
        @{ Name = 'scaled-monitor-clamp'; Width = 320; Height = 180; Minimum = "MinWidth=8192`r`nMinHeight=8192`r`n"; PolicyOnly = $true }
    )
}
foreach ($case in $cases) {
    # Fresh test-owned directory: never overwrite an INI or a DLL used by another
    # fixture. All these files are generated, synthetic, and remain under build/.
    $run = Join-Path (Join-Path $source 'test-runs') ([guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $run -Force | Out-Null
    foreach ($name in @('LakeWindowedFix.dll', 'LakeFixInjectionTest.exe', 'LakeFixFixture.exe', 'Contour.dll')) {
        [System.IO.File]::Copy((Join-Path $source $name), (Join-Path $run $name), $false)
    }
    # Legacy suites opt out explicitly now that scaling is the production default.
    $ini = "[window]`r`nWidth=$($case.Width)`r`nHeight=$($case.Height)`r`n$($case.Minimum)[compatibility]`r`nScaleRendering=$scaleValue`r`nNativeResize=$NativeResize`r`n"
    if ($ScaleRendering) { $ini += "[render]`r`nWidth=1536`r`nHeight=864`r`n" }
    if ($case.PolicyOnly) { $ini += "[fixture]`r`nWindowPolicyOnly=1`r`n" }
    [System.IO.File]::WriteAllText((Join-Path $run 'LakeWindowedFix.ini'), $ini, [System.Text.Encoding]::ASCII)
    $arguments = @('--app-dir', $run)
    if ($ExpectedExit -ge 0) { $arguments += @('--fixture-exit', "$ExpectedExit", '--wait') }
    Write-Host "Synthetic fixture $($case.Name): ScaleRendering=$scaleValue NativeResize=$NativeResize ExpectedExit=$ExpectedExit"
    & (Join-Path $run 'LakeFixInjectionTest.exe') @arguments
    if ($LASTEXITCODE -ne 0) { throw "Fixture falhou (exit $LASTEXITCODE). Logs em $run\logs" }
    Write-Host "Fixture reports: $run\logs"
}
