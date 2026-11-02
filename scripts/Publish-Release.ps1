#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [string]$PackageDirectory,
    [string]$GhPath = 'gh.exe',
    [switch]$DryRun,
    [switch]$Draft,
    [switch]$AllowUnsignedDevelopment
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Build-Provenance.ps1')
$root = Split-Path $PSScriptRoot -Parent
if (!$PackageDirectory) {
    if (!$Version) { throw 'Specify -Version or -PackageDirectory.' }
    $PackageDirectory = Join-Path $root "out\releases\v$Version"
}
$package = Assert-LakeSafePath (Resolve-Path -LiteralPath $PackageDirectory).Path
$manifest = Test-LakeReleasePackage -PackageDirectory $package -Version $Version -AllowUnsignedDevelopment:$AllowUnsignedDevelopment
if (!$Version) { $Version = $manifest.Version }
$product = Get-LakeProvenanceProductMetadata $root
if ($product.Version -cne $Version -or $product.AbiVersion -ne $manifest.Provenance.Runtime.AbiVersion -or
    $product.ConfigSchema -ne $manifest.Provenance.Runtime.ConfigSchema) { throw 'Package does not match the current central product metadata.' }
function Git-Checked([string[]]$Arguments) {
    $lines = @(& git -C $root @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Git failed: $($Arguments[0]). No force/update/delete is attempted." }
    return $lines
}
$head = (Git-Checked @('rev-parse','HEAD')) -join ''
if (@(Git-Checked @('status','--porcelain','--untracked-files=normal')).Count) { throw 'Commit and push all source changes before releasing.' }
if ($head -cnotmatch '^[a-f0-9]{40}$' -or $manifest.SourceDirty -or $manifest.SourceCommit -cne $head) {
    throw 'Publication requires this exact clean source commit. Candidate validation is separate.'
}
$expected = @("LakeWindowedFix-v$Version-win32.zip","LakeWindowedFix-Setup-v$Version.exe",'SHA256SUMS.txt')
function Get-GitHubRepository([string]$Url) {
    if ($Url -match '^git@github\.com:([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\.git)?$') { return $Matches[1] }
    if ($Url -match '^https://github\.com/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\.git)?/?$') { return $Matches[1] }
    throw 'origin must be an explicit GitHub repository, without embedded credentials.'
}
$repository = Get-GitHubRepository ((Git-Checked @('remote','get-url','origin')) -join '')
$pushUrls = @(Git-Checked @('remote','get-url','--push','--all','origin'))
if (!$pushUrls.Count) { throw 'origin has no effective push URL.' }
foreach ($url in $pushUrls) {
    if ((Get-GitHubRepository $url) -ine $repository) { throw 'Fetch and every push URL must target the same GitHub repository.' }
}
$ghRepository = "github.com/$repository"
$remoteHead = @(Git-Checked @('ls-remote','--heads','origin','refs/heads/main'))
if ($remoteHead.Count -ne 1 -or ($remoteHead[0] -split '\s+')[0] -ne $head) { throw 'Push this exact source commit to origin/main before releasing.' }
$tag = "v$Version"
$localTags = @(Git-Checked @('tag','--list',$tag))
if ($localTags.Count) {
    $localCommit = (Git-Checked @('rev-parse',"$tag^{commit}")) -join ''
    if ($localCommit -ne $head) { throw 'Local tag points to another commit; refusing to replace it.' }
}
$tagLines = @(Git-Checked @('ls-remote','--tags','origin',"refs/tags/$tag","refs/tags/$tag^{}"))
$remoteTagCommit = $null
if ($tagLines.Count) {
    $peeled = @($tagLines | Where-Object { $_.EndsWith('^{}') })
    $remoteTagCommit = (($peeled + $tagLines)[0] -split '\s+')[0]
    if ($remoteTagCommit -ne $head) { throw 'Existing remote tag points to another commit; refusing to replace it.' }
}
$assets = @($expected | ForEach-Object { Join-Path $package $_ }) + @(Join-Path $package 'RELEASE-MANIFEST.json')
if ($DryRun) {
    Write-Host "Release preflight approved: https://github.com/$repository/releases/tag/$tag"
    Write-Host "Commit $head; four MIT-only assets; experimental prerelease; no tag/release created."
    return
}
$gh = (Get-Command $GhPath -ErrorAction Stop).Source
& $gh auth status --hostname github.com
if ($LASTEXITCODE -ne 0) { throw 'Authenticate GitHub CLI before publishing; no tag was pushed.' }
$releaseJson = & $gh release list --repo $ghRepository --limit 1000 --json tagName
if ($LASTEXITCODE -ne 0) { throw 'Cannot query GitHub releases; no tag was pushed.' }
$existing = @($releaseJson | ConvertFrom-Json | Where-Object { $_.tagName -eq $tag })
if ($existing.Count) { throw 'Release already exists. No assets or release metadata will be overwritten.' }
if (!$remoteTagCommit) {
    if (!$localTags.Count) { Git-Checked @('tag','-a',$tag,$head,'-m',"Lake Controller Windowed Fix $Version experimental") | Out-Null }
    Git-Checked @('push','origin',"refs/tags/$tag") | Out-Null
}
$notesFolder = Join-Path $root ('private\release-notes\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $notesFolder | Out-Null
$notes = Join-Path $notesFolder 'notes.md'
[IO.File]::WriteAllText($notes,@"
## Somente patch / Patch only
**O software Lake Controller NAO acompanha este pacote.** Lake Controller, Contour e demais arquivos proprietarios nao sao distribuidos. E necessaria uma instalacao original legitima do **Lake Controller 5.8 Build 007**, com licenca quando exigida. Projeto independente, nao oficial nem endossado pelo fabricante. MIT aplica-se somente aos arquivos proprios do patch, nao ao software ou as marcas de terceiros.

**Lake Controller software is NOT included.** This package contains only the patch; Lake Controller, Contour and other proprietary files are not redistributed. You need a legitimate original **Lake Controller 5.8 Build 007** installation and a license where required. This is an independent, unofficial project, not endorsed by the manufacturer. MIT covers only the patch's own files, not third-party software or trademarks.

## Instalar / Install
- O atalho do patch usa um ícone próprio com faixa **PATCHED**. O atalho/ícone do Lake original permanece intacto para uso legado.
- The patch shortcut has its own **PATCHED** badge. The original Lake shortcut/icon stays intact for legacy use.
- Baixe **LakeWindowedFix-Setup-v$Version.exe**, feche o Lake, escolha **Portugues / English** no instalador e clique em **Instalar / Reparar**. Use o atalho **Lake Controller - Janela**. Instalacao somente no perfil do usuario; nao abre o Lake automaticamente.
- Download **LakeWindowedFix-Setup-v$Version.exe**, close Lake, choose **Portugues / English** in setup and click **Install / Repair**. Use the **Lake Controller - Janela** shortcut. Per-user installation; setup never starts Lake automatically.
- Se ja houver outra geracao do patch, use o setup externo com **--upgrade**. Para reverter a unica geracao anterior retida, **--rollback**. Nao remova recibos/journals manualmente para forcar a troca.
- For an existing different patch generation, run the external setup with **--upgrade**. Use **--rollback** for the single retained previous generation. Do not delete receipts/journals to force an update.
- Confira / Check **SHA256SUMS.txt** e / and **RELEASE-MANIFEST.json**. O ZIP portatil e opcional / The portable ZIP is optional.

## Limites / Limitations
**Experimental e nao assinado / Experimental and unsigned.** Siga a politica Windows/EDR; nao desative protecoes / Follow Windows/EDR policy; do not disable protections. Sem homologacao de audio/hardware, promessa de FPS ou vazamento corrigido / No audio/hardware approval, FPS promise or proven leak fix. Teste isolado sem equipamentos em operacao; OFFLINE nao isola a rede / Test in isolation without operational equipment; OFFLINE does not isolate the network.

O usuario relatou um teste local positivo do candidato anterior; nao e medicao independente nem validacao do instalador localizado ou do computador de destino. / The user reported a positive local smoke test of the previous candidate; this is not an independent measurement or validation of the localized installer or destination computer.

Fonte / Source: commit $head. [Portugues](https://github.com/$repository/blob/$tag/README.md) | [English](https://github.com/$repository/blob/$tag/README.en.md) | [Validation / Validacao](https://github.com/$repository/blob/$tag/docs/VALIDATION-07.md).
"@,[Text.UTF8Encoding]::new($false))
$arguments = @('release','create',$tag,'--repo',$ghRepository,'--verify-tag','--title',"Lake Controller Windowed Fix $Version (experimental)",
    '--notes-file',$notes,'--prerelease','--latest=false')
if ($Draft) { $arguments += '--draft' }
$arguments += $assets
& $gh @arguments
if ($LASTEXITCODE -ne 0) { throw 'Release creation failed. Existing tags/assets are never force-updated; inspect GitHub before retrying.' }
