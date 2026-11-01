#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version = '0.6.0',
    [string]$PackageDirectory,
    [string]$GhPath = 'gh.exe',
    [switch]$DryRun,
    [switch]$Draft
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$projectVersion = [regex]::Match([IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')),
    'project\(lake_controller_windowed_fix VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
if ($projectVersion -ne $Version) { throw 'VERSION does not match the project.' }
if (!$PackageDirectory) { $PackageDirectory = Join-Path $root "out\releases\v$Version" }
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$directory = Get-Item -LiteralPath $package
if (!$directory.PSIsContainer -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe package directory.' }
function Git-Checked([string[]]$Arguments) {
    $lines = @(& git -C $root @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Git failed: $($Arguments[0]). No force/update/delete is attempted." }
    return $lines
}
$head = (Git-Checked @('rev-parse','HEAD')) -join ''
if (@(Git-Checked @('status','--porcelain','--untracked-files=normal')).Count) { throw 'Commit and push all source changes before releasing.' }
$manifestPath = Join-Path $package 'RELEASE-MANIFEST.json'
$manifestFile = Get-Item -LiteralPath $manifestPath
if ($manifestFile.PSIsContainer -or ($manifestFile.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $manifestFile.Length -gt 1048576) { throw 'Unsafe release manifest.' }
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
$allowedFields = @('Schema','Project','Version','SourceCommit','SourceDirty','RuntimeFiles','Assets','Experimental','Notes')
$fields = @($manifest.PSObject.Properties.Name)
if ($fields.Count -ne $allowedFields.Count -or @($fields | Where-Object { $_ -notin $allowedFields }).Count) { throw 'Unexpected manifest metadata; publication refused to avoid leaking private fields.' }
$allowedNotes = @('MIT project files only; no proprietary Lake binaries or private evidence.',
    'Runtime tested OFFLINE only; no field/hardware approval or proven FPS/leak fix.',
    'Unsigned per-user installer; review Windows/EDR prompts according to local policy.')
if ((@($manifest.Notes) -join "`n") -cne ($allowedNotes -join "`n")) { throw 'Unexpected release notes metadata; use the generated manifest.' }
if ($manifest.Schema -ne 1 -or $manifest.Project -ne 'lake-controller-windowed-fix' -or $manifest.Version -ne $Version -or
    $manifest.SourceDirty -or $manifest.SourceCommit -ne $head -or $manifest.Experimental -ne $true) { throw 'Package must come from this exact clean source commit; run make package again.' }
$expected = @("LakeWindowedFix-v$Version-win32.zip","LakeWindowedFix-Setup-v$Version.exe",'SHA256SUMS.txt')
$seen = @()
foreach ($asset in $manifest.Assets) {
    $assetFields = @($asset.PSObject.Properties.Name)
    if ($assetFields.Count -ne 3 -or @($assetFields | Where-Object { $_ -notin @('Name','Sha256','Bytes') }).Count) { throw 'Unexpected asset metadata.' }
    if ($asset.Name -notin $expected -or $asset.Name -in $seen -or $asset.Sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Unsafe asset manifest.' }
    $seen += $asset.Name
    $path = Join-Path $package $asset.Name
    $file = Get-Item -LiteralPath $path
    if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (Get-FileHash -LiteralPath $path).Hash -ine $asset.Sha256 -or $file.Length -ne $asset.Bytes) { throw "Asset validation failed: $($asset.Name)" }
}
if ($seen.Count -ne 3) { throw 'Exactly three checksummed assets are required.' }
$checksumLines = foreach ($name in $expected[0..1]) {
    $record = @($manifest.Assets | Where-Object { $_.Name -eq $name })[0]
    "$($record.Sha256)  $name"
}
$expectedChecksums = ($checksumLines -join "`r`n") + "`r`n"
if ([IO.File]::ReadAllText((Join-Path $package 'SHA256SUMS.txt')) -cne $expectedChecksums) { throw 'Checksum contents do not match the two binary assets.' }
$runtimeNames = @('LakeWindowedLauncher.exe','LakeWindowedFix.dll','LakeWindowedFix.ini','LICENSE','Measure-LakeResources.ps1','LakeResourceMetrics.psm1')
if (@($manifest.RuntimeFiles).Count -ne 6 -or (@($manifest.RuntimeFiles) -join '|') -cne ($runtimeNames -join '|')) { throw 'Unexpected runtime file metadata.' }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $package "LakeWindowedFix-v$Version-win32.zip"))
try {
    $names = @($zip.Entries | ForEach-Object { $_.FullName })
    if ($names.Count -ne 6 -or @($names | Sort-Object -Unique).Count -ne 6 -or @($names | Where-Object { $_ -notin $runtimeNames }).Count) { throw 'ZIP contains unexpected files; publication refused.' }
} finally { $zip.Dispose() }
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
**Experimental, validacao limitada em OFFLINE. Nao homologado para uso em campo/audio de producao.** Nao ha ganho sustentado de FPS nem correcao de vazamento comprovados. Instalador nao assinado; siga a politica local de seguranca.

Os arquivos incluem somente o projeto MIT; nenhum binario proprietario, log, preset ou evidencia privada. O ZIP e a alternativa portatil para usuarios avancados. Confira **SHA256SUMS.txt** e **RELEASE-MANIFEST.json**.

Fonte: commit $head. [Instalacao e remocao](https://github.com/$repository/blob/$tag/docs/SETUP.md) | [Validacao](https://github.com/$repository/blob/$tag/docs/PERFORMANCE.md).
"@,[Text.UTF8Encoding]::new($false))
$arguments = @('release','create',$tag,'--repo',$ghRepository,'--verify-tag','--title',"Lake Controller Windowed Fix $Version (experimental)",
    '--notes-file',$notes,'--prerelease','--latest=false')
if ($Draft) { $arguments += '--draft' }
$arguments += $assets
& $gh @arguments
if ($LASTEXITCODE -ne 0) { throw 'Release creation failed. Existing tags/assets are never force-updated; inspect GitHub before retrying.' }
