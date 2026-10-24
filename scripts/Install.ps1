[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$AppDirectory,
    [string]$SourceDirectory = 'dist'
)
$ErrorActionPreference = 'Stop'
$app = (Resolve-Path -LiteralPath $AppDirectory).Path
$root = Split-Path $PSScriptRoot -Parent
$dist = if ([System.IO.Path]::IsPathRooted($SourceDirectory)) { $SourceDirectory } else { Join-Path $root $SourceDirectory }
$files = @('LakeWindowedLauncher.exe', 'LakeWindowedFix.dll', 'LakeWindowedFix.ini')
$manifest = Join-Path $app 'lake-windowed-fix.install.json'
$expected = @{
    'Lake Controller.exe' = 'e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713'
    'Contour.dll' = 'bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283'
}
foreach ($name in $expected.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $app $name) -Algorithm SHA256).Hash -ne $expected[$name]) {
        throw "Build desconhecido: $name. Nenhum arquivo copiado."
    }
}
if (Get-Process -Name 'Lake Controller' -ErrorAction SilentlyContinue) { throw 'Feche o Lake antes de instalar.' }
if (Test-Path -LiteralPath $manifest) { throw 'Instalacao existente. Desinstale-a antes de atualizar.' }
foreach ($name in $files) {
    if (!(Test-Path -LiteralPath (Join-Path $dist $name))) { throw "Compile primeiro: dist\$name ausente." }
    if (Test-Path -LiteralPath (Join-Path $app $name)) { throw "$name ja existe; nada sera sobrescrito." }
}
$created = [System.Collections.Generic.List[string]]::new()
try {
    $records = foreach ($name in $files) {
        $destination = Join-Path $app $name
        # overwrite=false is enforced atomically by File.Copy.
        [System.IO.File]::Copy((Join-Path $dist $name), $destination, $false)
        $created.Add($destination)
        @{ name = $name; sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash }
    }
    $json = @{ schema = 1; project = 'lake-controller-windowed-fix'; files = @($records) } | ConvertTo-Json -Depth 4
    $stream = [System.IO.File]::Open($manifest, [System.IO.FileMode]::CreateNew)
    $created.Add($manifest)
    try {
        $encoded = [System.Text.Encoding]::UTF8.GetBytes($json)
        $stream.Write($encoded, 0, $encoded.Length)
    } finally { $stream.Dispose() }
} catch {
    foreach ($path in $created) { Remove-Item -LiteralPath $path -ErrorAction Continue }
    throw
}
Write-Host "Instalado sem alterar arquivos originais. Inicie $app\LakeWindowedLauncher.exe"
