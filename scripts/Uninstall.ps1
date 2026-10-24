[CmdletBinding()]
param([Parameter(Mandatory)][string]$AppDirectory)
$ErrorActionPreference = 'Stop'
$app = (Resolve-Path -LiteralPath $AppDirectory).Path
$manifest = Join-Path $app 'lake-windowed-fix.install.json'
if (Get-Process -Name 'Lake Controller' -ErrorAction SilentlyContinue) { throw 'Feche o Lake antes de desinstalar.' }
$record = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
$allowed = @('LakeWindowedLauncher.exe', 'LakeWindowedFix.dll', 'LakeWindowedFix.ini')
if ($record.schema -ne 1 -or $record.project -ne 'lake-controller-windowed-fix' -or $record.files.Count -ne 3) {
    throw 'Manifesto desconhecido. Nenhum arquivo removido.'
}
$remove = @()
$seen = @()
foreach ($file in $record.files) {
    if ($file.name -notin $allowed -or $file.name -in $seen -or $file.sha256 -notmatch '^[a-fA-F0-9]{64}$') {
        throw 'Manifesto invalido. Nenhum arquivo removido.'
    }
    $seen += $file.name
    $path = Join-Path $app $file.name
    if (!(Test-Path -LiteralPath $path)) { continue }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
        if ($file.name -eq 'LakeWindowedFix.ini') { Write-Warning 'Configuracao editada preservada.'; continue }
        throw "Arquivo modificado: $path. Nenhum arquivo removido."
    }
    $remove += $path
}
foreach ($path in $remove) { Remove-Item -LiteralPath $path }
Remove-Item -LiteralPath $manifest
Write-Host 'Patch removido. O executavel e as DLLs originais nao foram alterados.'
