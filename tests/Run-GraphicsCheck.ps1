[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryDirectory,
    [ValidateSet('Automated', 'Manual')][string]$Mode = 'Automated',
    [ValidateRange(320, 4096)][int]$CanvasWidth = 1088,
    [ValidateRange(180, 4096)][int]$CanvasHeight = 612,
    [switch]$ConfirmActiveUnlockedDesktop,
    [switch]$ConfirmNoOperationalAudio,
    [string]$ReportDirectory
)
$ErrorActionPreference = 'Stop'
$binary = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$exe = Join-Path $binary 'LakeFixGraphicsCheck.exe'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Diagnostico ausente: $exe" }
if (!$ReportDirectory) { $ReportDirectory = Join-Path $binary 'graphics-check-runs' }
$parent = [IO.Path]::GetFullPath($ReportDirectory)
$run = Join-Path $parent (('{0:yyyyMMdd-HHmmss}-' -f [DateTime]::UtcNow) + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$report = Join-Path $run 'graphics-check.log'

# This script does not infer an interactive/unlocked desktop from the mere
# presence of Explorer, a monitor, a console, or a successfully built EXE.
# Without the operator's explicit attestation, do not start the executable.
if (!$ConfirmActiveUnlockedDesktop -or !$ConfirmNoOperationalAudio) {
    $text = "summary=NOT_EXECUTED exit=2 graphics_executed=0`r`n" +
            "reason=operator did not guarantee BOTH an active unlocked desktop and no operational audio session`r`n" +
            "scope=own diagnostic only; not a validation of Lake, proprietary renderer or audio`r`n"
    [IO.File]::WriteAllText($report, $text, [Text.UTF8Encoding]::new($false))
    Write-Warning "NOT_EXECUTED: desktop ativo/desbloqueado e ausencia de audio operacional nao garantidos; EXE nao iniciado. Relatorio: $report"
    exit 2
}

$arguments = @("--$($Mode.ToLowerInvariant())", '--desktop-confirmed', '--no-operational-audio-confirmed', '--width', "$CanvasWidth",
               '--height', "$CanvasHeight", '--report', $report)
Write-Host "Diagnostico independente ($Mode): somente janela/surfaces proprias. Sem Lake/audio/input global."
& $exe @arguments
$result = $LASTEXITCODE
if (!(Test-Path -LiteralPath $report -PathType Leaf)) { throw "Diagnostico nao produziu relatorio (exit $result): $report" }
Write-Host "Relatorio: $report"
if ($result -eq 0) {
    Write-Host 'PASS restrito ao diagnostico proprio e aos pixels/caminhos registrados; NAO aprova Lake ou audio.'
} elseif ($result -eq 2) {
    Write-Warning 'NOT_EXECUTED: precondicoes/desktop nao estabelecidos; nao e aprovacao grafica.'
} elseif ($result -eq 3) {
    Write-Warning 'INCONCLUSIVE: consulte oclusao/readback/desktop no relatorio; nao e aprovacao grafica.'
} else {
    Write-Warning "FAIL: diagnostico retornou $result; preserve o relatorio."
}
exit $result
