[CmdletBinding()]
param([Parameter(Mandatory)][string]$AppDirectory)
$ErrorActionPreference = 'Stop'
$app = (Resolve-Path -LiteralPath $AppDirectory).Path
$expected = @{
    'Lake Controller.exe' = 'e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713'
    'Contour.dll' = 'bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283'
}
foreach ($name in $expected.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $app $name) -Algorithm SHA256).Hash -ne $expected[$name]) {
        throw "Build desconhecido: $name. Nenhum processo iniciado."
    }
}
Write-Warning 'Diagnostico sem DLL: as flags nativas nao acrescentam, sozinhas, bordas ou resize padrao.'
Start-Process -FilePath (Join-Path $app 'Lake Controller.exe') -ArgumentList @('windowed', 'nonfullscreen') -WorkingDirectory $app -PassThru
