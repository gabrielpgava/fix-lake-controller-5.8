#Requires -Version 5.1
# Validate exact own final PEs/PDBs without loading/executing them. Mutations use
# fresh ignored copies only; no tool, installation, registry or network access.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SymbolDirectory,[Parameter(Mandatory)][string]$BinaryDirectory,
    [Parameter(Mandatory)][string]$BuildId,[switch]$ValidateOnly)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'scripts/Preserve-OwnSymbols.ps1')
$store=Get-LakeSymbolSafePath $SymbolDirectory
$binary=Get-LakeSymbolSafePath $BinaryDirectory
$checks=0
function Require-Link([bool]$Condition,[string]$Message) { if (!$Condition) { throw $Message } }
function Reject-Link([scriptblock]$Action) {
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    Require-Link $rejected 'Invalid symbol association unexpectedly passed.'
}
function Verify-Link([string]$Directory,[string]$Output) {
    Test-LakeOwnLinkSymbolRecord -RecordFile (Join-Path $Directory ([IO.Path]::GetFileName($Output)+'.link-symbols.json')) -OutputFile $Output -BuildId $BuildId
}
$records=@(Get-ChildItem -LiteralPath $store -Filter '*.link-symbols.json' -File)
Require-Link ($records.Count -ge 1) 'No own same-link symbol records found.'
foreach ($file in $records) {
    $name=$file.Name.Substring(0,$file.Name.Length-'.link-symbols.json'.Length)
    $record=Verify-Link $store (Join-Path $binary $name)
    Write-Host "PASS: $name exact final PE, private original/split receipt and same-link PDB GUID/age/hash; build=$BuildId"
    $checks++
    if ($ValidateOnly) { continue }
    $sandbox=Join-Path $root ('build-link-symbol-tests/'+[guid]::NewGuid().ToString('N'))
    $mutations=@('final','original','receipt','build','missing-pdb','pdb-hash','pdb-guid','pdb-age','pdb-magic','pdb-range','pdb-info')
    foreach ($kind in $mutations) {
        if (!$record.Pdb -and $kind -match '^pdb-|^missing-pdb$') { continue }
        $copy=Join-Path $sandbox ($kind+'/'+$BuildId)
        $outputs=Join-Path $sandbox ($kind+'/final')
        [IO.Directory]::CreateDirectory($copy) | Out-Null
        [IO.Directory]::CreateDirectory($outputs) | Out-Null
        foreach ($input in @(Get-ChildItem -LiteralPath $store -File)) { [IO.File]::Copy($input.FullName,(Join-Path $copy $input.Name),$false) }
        $output=Join-Path $outputs $name
        [IO.File]::Copy((Join-Path $binary $name),$output,$false)
        $receipt=Join-Path $copy $file.Name
        $mutable=[IO.File]::ReadAllText($receipt) | ConvertFrom-Json
        if ($kind -cin @('final','original','pdb-hash')) {
            $path=if ($kind -eq 'final') { $output } elseif ($kind -eq 'original') { Join-Path $copy $name } else { Join-Path $copy $record.Pdb.Name }
            $bytes=[IO.File]::ReadAllBytes($path); $bytes[$bytes.Length-1]=$bytes[$bytes.Length-1] -bxor 1; [IO.File]::WriteAllBytes($path,$bytes)
        } elseif ($kind -eq 'receipt') { $mutable | Add-Member -NotePropertyName Unknown -NotePropertyValue 'refuse' }
        elseif ($kind -eq 'build') { $mutable.BuildId=[guid]::NewGuid().ToString('N') }
        elseif ($kind -eq 'missing-pdb') { [IO.File]::Move((Join-Path $copy $record.Pdb.Name),(Join-Path $copy 'missing.pdb')) }
        else {
            $path=Join-Path $copy $record.Pdb.Name
            $bytes=[IO.File]::ReadAllBytes($path)
            $block=[BitConverter]::ToUInt32($bytes,32); $map=[BitConverter]::ToUInt32($bytes,52)*$block
            $directory=[BitConverter]::ToUInt32($bytes,$map)*$block
            $count=[BitConverter]::ToUInt32($bytes,$directory)
            $firstSize=[BitConverter]::ToUInt32($bytes,$directory+4)
            $skip=if ($firstSize -eq [uint32]::MaxValue) { 0 } else { [int][Math]::Ceiling($firstSize/[double]$block) }
            $info=[BitConverter]::ToUInt32($bytes,$directory+4+4*$count+4*$skip)*$block
            $offset=switch ($kind) { 'pdb-guid' {$info+12} 'pdb-age' {$info+8} 'pdb-magic' {0} 'pdb-range' {40} 'pdb-info' {$directory+8} }
            if ($kind -eq 'pdb-info') { [Array]::Copy([BitConverter]::GetBytes([uint32]0),0,$bytes,$offset,4) }
            else { $bytes[$offset]=$bytes[$offset] -bxor 1 }
            [IO.File]::WriteAllBytes($path,$bytes)
            # Rehash deliberately: GUID/age and structural checks must reject
            # the mismatch, not only the outer file digest.
            $mutable.Pdb=Get-LakeLinkSymbolFileRecord $path
        }
        [IO.File]::WriteAllText($receipt,($mutable | ConvertTo-Json -Depth 8)+"`n",[Text.UTF8Encoding]::new($false))
        Reject-Link { Verify-Link $copy $output }
        Write-Host "PASS: $name rejects $kind tampering/missing association"
        $checks++
    }
}
Write-Host "Own link symbols: $checks passed; no product PE was loaded or executed."
