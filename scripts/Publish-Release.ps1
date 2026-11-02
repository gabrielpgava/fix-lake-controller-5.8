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
## Instalar
Baixe **LakeWindowedFix-Setup-v$Version.exe**, abra com duplo clique, clique em **Instalar** e use o atalho **Lake Controller - Janela**.
Requer Lake Controller 5.8 Build 007 original instalado. Sem comandos ou edicao manual de INI; instalacao apenas no perfil do usuario.

## Limites
**Candidata experimental; sem validacao real Lake OFFLINE desta candidata. Nao homologado para uso em campo/audio de producao.** Nao ha ganho sustentado de FPS nem correcao de vazamento comprovados. Instalador nao assinado; siga a politica local de seguranca.

Os arquivos incluem somente o projeto MIT; nenhum binario proprietario, log, preset ou evidencia privada. O ZIP e a alternativa portatil para usuarios avancados. Confira **SHA256SUMS.txt** e **RELEASE-MANIFEST.json**.

Fonte: commit $head. [Instalacao e remocao](https://github.com/$repository/blob/$tag/docs/SETUP.md) | [Validacao](https://github.com/$repository/blob/$tag/docs/PERFORMANCE.md).
"@,[Text.UTF8Encoding]::new($false))
$arguments = @('release','create',$tag,'--repo',$ghRepository,'--verify-tag','--title',"Lake Controller Windowed Fix $Version (experimental)",
    '--notes-file',$notes,'--prerelease','--latest=false')
if ($Draft) { $arguments += '--draft' }
$arguments += $assets
& $gh @arguments
if ($LASTEXITCODE -ne 0) { throw 'Release creation failed. Existing tags/assets are never force-updated; inspect GitHub before retrying.' }
