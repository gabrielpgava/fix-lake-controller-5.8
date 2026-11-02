#Requires -Version 5.1
# Reads/copies already-built OWN PEs; never executes/loads those images. Only
# local llvm-objcopy is executed. All mutations remain in a fresh ignored sandbox.
[CmdletBinding()]
param([string]$InputDirectory,[string]$BuildId,[string]$ObjcopyPath='auto',
    [string]$OutputRoot='build-symbols-next')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$library = Join-Path $root 'scripts\Split-OwnSymbols.ps1'
. $library

$script:symbolPassed = 0; $script:symbolFailed = 0
function Require-Symbol([bool]$Condition,[string]$Message) { if (!$Condition) { throw $Message } }
function Reject-Symbol([scriptblock]$Action,[string]$MessagePattern='.') {
    $caught = $null
    try { & $Action | Out-Null } catch { $caught = $_ }
    Require-Symbol ($null -ne $caught) 'Unsafe operation unexpectedly passed.'
    Require-Symbol ($caught.Exception.Message -match $MessagePattern) "Unexpected rejection: $($caught.Exception.Message)"
}
function Case-Symbol([string]$CaseLabel,[scriptblock]$Action) {
    try { & $Action; $script:symbolPassed++; Write-Host "PASS: $CaseLabel" }
    catch { $script:symbolFailed++; Write-Host "FAIL: $CaseLabel - $($_.Exception.Message)"; Write-Host $_.ScriptStackTrace }
}
function Copy-SymbolNew([string]$Source,[string]$Destination) {
    [IO.Directory]::CreateDirectory((Split-Path $Destination -Parent)) | Out-Null
    $input = [IO.File]::Open($Source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try {
        $output = [IO.File]::Open($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $input.CopyTo($output) } finally { $output.Dispose() }
    } finally { $input.Dispose() }
}
function New-SymbolFixture([string]$Name='LakeWindowedFix.dll',[string]$Source=$script:inputs['LakeWindowedFix.dll']) {
    $folder = Join-Path $sandbox ([guid]::NewGuid().ToString('N'))
    $input = Join-Path $folder ('input\'+$Name)
    Copy-SymbolNew $Source $input
    [pscustomobject]@{Root=$folder;Input=$input;Output=(Join-Path $folder ('output\'+$Name))
        Symbols=(Join-Path $folder ('symbols\'+$script:identity));Name=$Name}
}
function Split-SymbolFixture($Fixture,[string]$Tool=$script:tool.Path) {
    Split-LakeOwnSymbols -InputFile $Fixture.Input -OutputFile $Fixture.Output -SymbolDirectory $Fixture.Symbols `
        -BuildId $script:identity -ObjcopyPath $Tool
}
function Copy-SymbolBundle($Source) {
    $copy = New-SymbolFixture
    Copy-SymbolNew $Source.Output $copy.Output
    foreach ($name in @($Source.Name,($Source.Name+'.debug'),($Source.Name+'.symbols.json'))) {
        Copy-SymbolNew (Join-Path $Source.Symbols $name) (Join-Path $copy.Symbols $name)
    }
    return $copy
}
function Verify-SymbolFixture($Fixture) {
    Test-LakeOwnSymbolRecord -RecordFile (Join-Path $Fixture.Symbols ($Fixture.Name+'.symbols.json')) `
        -OutputFile $Fixture.Output -BuildId $script:identity
}
function Set-SymbolByte([string]$Path,[long]$Offset) {
    $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try {
        $stream.Position = $Offset; $value = $stream.ReadByte()
        if ($value -lt 0) { throw 'Fixture mutation outside file.' }
        $stream.Position = $Offset; $stream.WriteByte([byte]($value -bxor 1))
    } finally { $stream.Dispose() }
}
function Set-SymbolUInt32([string]$Path,[long]$Offset,[uint32]$Value) {
    $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { $stream.Position = $Offset; $bytes = [BitConverter]::GetBytes($Value); $stream.Write($bytes,0,4) }
    finally { $stream.Dispose() }
}
function Update-SymbolRecord($Fixture,[scriptblock]$Edit) {
    $receiptPath = Join-Path $Fixture.Symbols ($Fixture.Name+'.symbols.json')
    $receiptValue = [IO.File]::ReadAllText($receiptPath) | ConvertFrom-Json
    & $Edit $receiptValue
    [IO.File]::WriteAllText($receiptPath,($receiptValue | ConvertTo-Json -Depth 8)+"`n",[Text.UTF8Encoding]::new($false))
}
function Get-SymbolRuntimeGuid([string]$Path) {
    $layout = Get-LakeOwnPeLayout $Path
    $stream = [IO.File]::OpenRead($Path); $reader = [IO.BinaryReader]::new($stream)
    try {
        $ids = @()
        foreach ($section in @($layout.Sections | Where-Object { !$_.Debug -and $_.RawSize -and ($_.Characteristics -band 0x40000000) })) {
            $stream.Position = $section.RawOffset; $bytes = $reader.ReadBytes([int]$section.RawSize)
            foreach ($text in @([Text.Encoding]::ASCII.GetString($bytes),[Text.Encoding]::Unicode.GetString($bytes))) {
                foreach ($match in [regex]::Matches($text,'(?<![A-Za-z0-9])([a-fA-F0-9]{32}|[a-fA-F0-9]{8}(?:-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12})\x00')) {
                    $ids += $match.Groups[1].Value
                }
            }
        }
        $ids = @($ids | Sort-Object -Unique)
        if ($ids.Count -ne 1) { throw 'Supply -BuildId: fixture has no unique runtime GUID.' }
        return $ids[0]
    } finally { $reader.Dispose(); $stream.Dispose() }
}

if (!$InputDirectory) {
    foreach ($candidate in @('build-validation-zig','build-integration')) {
        if (Test-Path -LiteralPath (Join-Path $root "$candidate\LakeWindowedFix.dll") -PathType Leaf) {
            $InputDirectory = Join-Path $root $candidate; break
        }
    }
}
if (!$InputDirectory) { throw 'Build own optimized Zig PEs first, or pass -InputDirectory and -BuildId. No compilation/download is performed here.' }
$InputDirectory = Get-LakeSymbolSafePath $InputDirectory
$sandbox = Join-Path (Get-LakeSymbolSafePath $OutputRoot) ('tests-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($sandbox) | Out-Null
$script:inputs = @{}; $sourceHashes = @{}
foreach ($name in @('LakeWindowedFix.dll','LakeWindowedLauncher.exe')) {
    $source = Join-Path $InputDirectory $name
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        $sourceHashes[$source] = Get-LakeSymbolHash $source
        $snapshot = Join-Path $sandbox ('inputs\'+$name)
        Copy-SymbolNew $source $snapshot
        Require-Symbol ((Get-LakeSymbolHash $snapshot) -ceq $sourceHashes[$source]) 'Source changed while copying fixture.'
        $script:inputs[$name] = $snapshot
    }
}
if (!$script:inputs.ContainsKey('LakeWindowedFix.dll')) { throw 'The test suite needs the own runtime DLL as a fixture.' }
$script:identity = if ($BuildId) { $BuildId } else { Get-SymbolRuntimeGuid $script:inputs['LakeWindowedFix.dll'] }
Assert-LakeSymbolBuildId $script:identity
$script:tool = Resolve-LakeSymbolObjcopy $ObjcopyPath
$good = $null

Case-Symbol 'Dot-source contains definitions only and leaves the caller preferences unchanged' {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($library,[ref]$tokens,[ref]$errors)
    Require-Symbol (!$errors.Count) 'PowerShell library syntax error.'
    Require-Symbol (!@($ast.EndBlock.Statements | Where-Object { $_ -isnot [Management.Automation.Language.FunctionDefinitionAst] }).Count) 'Library has executable import statements.'
    $preference = $ErrorActionPreference
    $result = @(. $library)
    Require-Symbol (!$result.Count -and $ErrorActionPreference -ceq $preference) 'Import had side effects/output.'
}

foreach ($name in @('LakeWindowedFix.dll','LakeWindowedLauncher.exe')) {
    if (!$script:inputs.ContainsKey($name)) { continue }
    Case-Symbol "Real optimized own $name splits without executing the image" {
        $fixture = New-SymbolFixture $name $script:inputs[$name]
        $preHash = Get-LakeSymbolHash $fixture.Input
        $record = Split-SymbolFixture $fixture -Tool $ObjcopyPath
        $before = Get-LakeOwnPeLayout $fixture.Input -BuildId $script:identity
        $after = Get-LakeOwnPeLayout $fixture.Output -BuildId $script:identity
        Require-Symbol ($record.BuildId -ceq $script:identity -and $record.PreSha256 -ceq $preHash -and
            (Get-LakeSymbolHash $fixture.Input) -ceq $preHash) 'Input/BuildId changed.'
        Require-Symbol ($record.PostBytes -lt $record.PreBytes -and !@($after.Sections | Where-Object { $_.Debug }).Count) 'No real size/debug-section reduction.'
        Require-Symbol ($record.Machine -eq 332 -and $after.EntryRva -eq $before.EntryRva -and $after.ImageBase -eq $before.ImageBase -and
            $after.CodeSha256 -ceq $before.CodeSha256) 'Entry/base/x86/code identity changed.'
        Assert-LakeSymbolEquivalent $before $after
        Require-Symbol ($record.ToolSha256 -ceq $script:tool.Sha256 -and $record.ToolVersion -ceq $script:tool.Version) 'Wrong tool identity.'
        $verified = Verify-SymbolFixture $fixture
        Require-Symbol ($verified.PostSha256 -ceq $record.PostSha256) 'Revalidation changed the receipt.'
        $json = [IO.File]::ReadAllText((Join-Path $fixture.Symbols ($name+'.symbols.json')))
        Require-Symbol ($json -notmatch '[A-Za-z]:[\\/]' -and !$json.Contains($root) -and !$json.Contains($sandbox)) 'Private path leaked into metadata.'
        Write-Host "MEASURE $name pre=$($record.PreBytes) post=$($record.PostBytes) debug=$($record.DebugBytes) entry=$($record.EntryRva) code=$($record.CodeSha256)"
        if ($name -ceq 'LakeWindowedFix.dll') { $script:good = $fixture }
    }
}
if (!$good) { throw 'Baseline split failed; preserve sandbox, cannot validate mutations.' }

Case-Symbol 'Real objcopy argv preserves spaces in input, output and symbol-store paths' {
    $folder = Join-Path $sandbox 'paths with spaces'
    $f = [pscustomobject]@{Root=$folder;Name='LakeWindowedFix.dll'
        Input=(Join-Path $folder 'own inputs\LakeWindowedFix.dll')
        Output=(Join-Path $folder 'public candidate\LakeWindowedFix.dll')
        Symbols=(Join-Path $folder ('own symbols\'+$script:identity))}
    Copy-SymbolNew $script:inputs['LakeWindowedFix.dll'] $f.Input
    $record = Split-SymbolFixture $f
    Require-Symbol ((Verify-SymbolFixture $f).PostSha256 -ceq $record.PostSha256) 'Spaced paths changed objcopy argv/output.'
}
Case-Symbol 'COFF /decimal long names resolve .debug_frame rather than silently missing debug data' {
    $layout = Get-LakeOwnPeLayout $good.Input
    $debug = @($layout.Sections | Where-Object { $_.Debug })
    Require-Symbol ($debug.Count -gt 0 -and @($debug | Where-Object { $_.RawName -match '^/[0-9]+$' }).Count -gt 0) 'Fixture lacks a real COFF long debug name.'
    Require-Symbol ($layout.StringTableOffset -gt 0 -and $debug[0].Name.StartsWith('.debug')) 'COFF string table not decoded.'
}
Case-Symbol 'Wrong build GUID fails before publication, without relabeling an old image' {
    $f = New-SymbolFixture; $wrong = [guid]::NewGuid().ToString('N')
    Reject-Symbol { Split-LakeOwnSymbols $f.Input $f.Output (Join-Path $f.Root "symbols\$wrong") $wrong -ObjcopyPath $script:tool.Path } 'Exact build GUID is absent'
    Require-Symbol (!(Test-Path -LiteralPath $f.Output)) 'Mismatched build was published.'
}
Case-Symbol 'Empty/non-GUID BuildId and a symbols directory for another GUID are refused' {
    $f = New-SymbolFixture
    foreach ($id in @('development-untracked',('0'*32),'HEAD','../private')) {
        Reject-Symbol { Split-LakeOwnSymbols $f.Input $f.Output $f.Symbols $id -ObjcopyPath $script:tool.Path }
    }
    Reject-Symbol { Split-LakeOwnSymbols $f.Input $f.Output (Join-Path $f.Root 'wrong-store') $script:identity } 'exact BuildId'
}
Case-Symbol 'Proprietary, fixture and foreign basenames are rejected even with own PE contents' {
    foreach ($name in @('Contour.dll','Lake Controller.exe','StdGraphics.dll','DaylightGraphics.dll',
        'LakeWindowedFix.Test.dll','LakeFixFixture.exe','foreign.dll','LakeWindowedFix-Setup-v0.7.0.exe.bak')) {
        $f = New-SymbolFixture $name
        Reject-Symbol { Split-SymbolFixture $f } 'Not an admitted own product filename'
        Require-Symbol (!(Test-Path -LiteralPath $f.Output)) 'Foreign basename published.'
    }
}
Case-Symbol 'Renaming a foreign/stale runtime cannot use a BuildId present only outside runtime sections' {
    $f = New-SymbolFixture
    $layout = Get-LakeOwnPeLayout $f.Input
    $bytes = [IO.File]::ReadAllBytes($f.Input); $mutations = 0
    foreach ($section in @($layout.Sections | Where-Object { !$_.Debug -and $_.RawSize })) {
        $text = [Text.Encoding]::ASCII.GetString($bytes,[int]$section.RawOffset,[int]$section.RawSize)
        $position = 0
        while (($position = $text.IndexOf($script:identity+[char]0,$position,[StringComparison]::Ordinal)) -ge 0) {
            $bytes[[int]$section.RawOffset+$position] = [byte][char]'X'; $mutations++; $position++
        }
        $text = [Text.Encoding]::Unicode.GetString($bytes,[int]$section.RawOffset,[int]$section.RawSize)
        $position = 0
        while (($position = $text.IndexOf($script:identity+[char]0,$position,[StringComparison]::Ordinal)) -ge 0) {
            $bytes[[int]$section.RawOffset+2*$position] = [byte][char]'X'; $mutations++; $position++
        }
    }
    Require-Symbol ($mutations -gt 0) 'No runtime identity mutated.'
    [IO.File]::WriteAllBytes($f.Input,$bytes)
    $append = [IO.File]::Open($f.Input,[IO.FileMode]::Append,[IO.FileAccess]::Write)
    try { $idBytes = [Text.Encoding]::ASCII.GetBytes($script:identity+[char]0); $append.Write($idBytes,0,$idBytes.Length) }
    finally { $append.Dispose() }
    Reject-Symbol { Split-SymbolFixture $f } 'Exact build GUID is absent'
}
Case-Symbol 'Existing output is preserved byte for byte, before any objcopy resolution' {
    $f = New-SymbolFixture
    [IO.Directory]::CreateDirectory((Split-Path $f.Output -Parent)) | Out-Null
    [IO.File]::WriteAllText($f.Output,'existing user output; preserve')
    $hash = Get-LakeSymbolHash $f.Output
    Reject-Symbol { Split-SymbolFixture $f -Tool (Join-Path $f.Root 'missing-tool.exe') } 'Existing output'
    Require-Symbol ((Get-LakeSymbolHash $f.Output) -ceq $hash -and !(Test-Path -LiteralPath $f.Symbols)) 'Existing destination was changed.'
}
Case-Symbol 'Input equals output cannot strip or replace the original' {
    $f = New-SymbolFixture; $hash = Get-LakeSymbolHash $f.Input
    Reject-Symbol { Split-LakeOwnSymbols $f.Input $f.Input $f.Symbols $script:identity } 'NEW output'
    Require-Symbol ((Get-LakeSymbolHash $f.Input) -ceq $hash) 'Original was modified.'
}
foreach ($suffix in @('','.debug','.symbols.json')) {
    Case-Symbol "Existing symbol artifact '$suffix' is preserved; no partial public output" {
        $f = New-SymbolFixture
        [IO.Directory]::CreateDirectory($f.Symbols) | Out-Null
        $path = Join-Path $f.Symbols ($f.Name+$suffix)
        [IO.File]::WriteAllText($path,'existing symbol data; preserve'); $hash = Get-LakeSymbolHash $path
        Reject-Symbol { Split-SymbolFixture $f } 'Existing output/symbol artifact'
        Require-Symbol ((Get-LakeSymbolHash $path) -ceq $hash -and !(Test-Path -LiteralPath $f.Output)) 'Symbol collision overwritten.'
    }
}
Case-Symbol 'Already-stripped input is not relabeled as a new DWARF split' {
    $f = New-SymbolFixture -Source $good.Output
    Reject-Symbol { Split-SymbolFixture $f } 'No embedded DWARF'
}
Case-Symbol 'Missing explicit tool fails without fallback download or published output' {
    $f = New-SymbolFixture
    Reject-Symbol { Split-SymbolFixture $f -Tool (Join-Path $f.Root 'missing-objcopy.exe') }
    Require-Symbol (!(Test-Path -LiteralPath $f.Output) -and !(Test-Path -LiteralPath $f.Symbols)) 'Failed tool published data.'
}
Case-Symbol 'Malformed COFF long-name offset is rejected within its declared string table' {
    $f = New-SymbolFixture; $layout = Get-LakeOwnPeLayout $f.Input
    $section = @($layout.Sections | Where-Object { $_.Debug })[0]
    $stream = [IO.File]::OpenWrite($f.Input)
    try { $stream.Position = $section.HeaderOffset; $name = [Text.Encoding]::ASCII.GetBytes('/9999999'); $stream.Write($name,0,8) }
    finally { $stream.Dispose() }
    Reject-Symbol { Split-SymbolFixture $f } 'outside the string table'
}
Case-Symbol 'Malformed PE header offset cannot cause unbounded/out-of-file reads' {
    $f = New-SymbolFixture
    Set-SymbolUInt32 $f.Input 60 ([uint32]::MaxValue)
    Reject-Symbol { Split-SymbolFixture $f } 'Invalid PE header range'
}
Case-Symbol 'Executable section disguised as debug is never discarded' {
    $f = New-SymbolFixture; $layout = Get-LakeOwnPeLayout $f.Input
    $section = @($layout.Sections | Where-Object { $_.Debug })[0]
    Set-SymbolUInt32 $f.Input ($section.HeaderOffset+36) 0x60000020
    Reject-Symbol { Split-SymbolFixture $f } 'Executable section mislabeled'
}
Case-Symbol 'Certificate-bearing PE is refused rather than invalidating an existing signature' {
    $f = New-SymbolFixture; $layout = Get-LakeOwnPeLayout $f.Input
    Set-SymbolUInt32 $f.Input ($layout.OptionalOffset+96+8*4) 512
    Set-SymbolUInt32 $f.Input ($layout.OptionalOffset+96+8*4+4) 8
    Reject-Symbol { Split-SymbolFixture $f } 'Signed/certificate-bearing'
    Require-Symbol (!(Test-Path -LiteralPath $f.Output)) 'Signed fixture was published.'
}

foreach ($kind in @('code','rdata','entry','base','rva','machine')) {
    Case-Symbol "Tampered stripped $kind is detected even after updating its receipt SHA" {
        $f = Copy-SymbolBundle $good; $layout = Get-LakeOwnPeLayout $f.Output
        switch ($kind) {
            'code' { $section = @($layout.Sections | Where-Object { $_.Executable })[0]; Set-SymbolByte $f.Output $section.RawOffset }
            'rdata' { $section = @($layout.Sections | Where-Object { $_.Name -ceq '.rdata' })[0]; Set-SymbolByte $f.Output ([long]$section.RawOffset+$section.RawSize-1) }
            'entry' { Set-SymbolUInt32 $f.Output ($layout.OptionalOffset+16) ($layout.EntryRva+1) }
            'base' { Set-SymbolUInt32 $f.Output ($layout.OptionalOffset+28) ($layout.ImageBase+65536) }
            'rva' { $section = @($layout.Sections | Where-Object { $_.Executable })[0]; Set-SymbolUInt32 $f.Output ($section.HeaderOffset+12) ($section.Rva+16) }
            'machine' {
                $stream = [IO.File]::OpenWrite($f.Output)
                try { $stream.Position = $layout.PeOffset+4; $stream.WriteByte(0x64); $stream.WriteByte(0x86) }
                finally { $stream.Dispose() }
            }
        }
        Update-SymbolRecord $f { param($r) $r.PostSha256 = Get-LakeSymbolHash $f.Output }
        Reject-Symbol { Verify-SymbolFixture $f } 'changed|mutated|Only own PE x86|outside|Overlapping'
    }
}
Case-Symbol 'Tampered debug bytes are detected even after updating DebugSha256' {
    $f = Copy-SymbolBundle $good; $path = Join-Path $f.Symbols ($f.Name+'.debug')
    $layout = Get-LakeOwnPeLayout $path; $section = @($layout.Sections | Where-Object { $_.Debug })[0]
    Set-SymbolByte $path $section.RawOffset
    Update-SymbolRecord $f { param($r) $r.DebugSha256 = Get-LakeSymbolHash $path }
    Reject-Symbol { Verify-SymbolFixture $f } 'changed original DWARF'
}
Case-Symbol 'A full image cannot masquerade as only-keep-debug by changing the receipt' {
    $f = Copy-SymbolBundle $good
    $path = Join-Path $f.Symbols ($f.Name+'.debug')
    [IO.File]::WriteAllBytes($path,[IO.File]::ReadAllBytes((Join-Path $f.Symbols $f.Name)))
    Update-SymbolRecord $f { param($r) $r.DebugSha256 = $r.PreSha256; $r.DebugBytes = $r.PreBytes }
    Reject-Symbol { Verify-SymbolFixture $f } 'still contains executable raw bytes'
}
Case-Symbol 'Changed preserved unstripped input cannot reuse the exact receipt' {
    $f = Copy-SymbolBundle $good; $path = Join-Path $f.Symbols $f.Name
    Set-SymbolByte $path 2
    Reject-Symbol { Verify-SymbolFixture $f } 'changed after separation'
}
Case-Symbol 'Missing debug sidecar prevents receipt verification' {
    $f = Copy-SymbolBundle $good; $path = Join-Path $f.Symbols ($f.Name+'.debug')
    [IO.File]::Move($path,($path+'.saved'))
    Reject-Symbol { Verify-SymbolFixture $f }
}
foreach ($field in @('BuildId','CodeSha256','EntryRva','DebugName','ToolVersion')) {
    Case-Symbol "Receipt tampering in $field is rejected" {
        $f = Copy-SymbolBundle $good
        Update-SymbolRecord $f {
            param($r)
            switch ($field) {
                'BuildId' { $r.BuildId = [guid]::NewGuid().ToString('N') }
                'CodeSha256' { $r.CodeSha256 = '0'*64 }
                'EntryRva' { $r.EntryRva++ }
                'DebugName' { $r.DebugName = '..\private\foreign.debug' }
                'ToolVersion' { $r.ToolVersion = 'C:\private\tool.exe' }
            }
        }
        Reject-Symbol { Verify-SymbolFixture $f } 'Invalid|Unexpected|altered'
    }
}
Case-Symbol 'Injected objcopy code mutation is refused before any public output/archive promotion' {
    $f = New-SymbolFixture; $hash = Get-LakeSymbolHash $f.Input
    $script:realSymbolInvoker = ${function:Invoke-LakeSymbolObjcopy}
    function Invoke-LakeSymbolObjcopy([string]$Tool,[string[]]$Arguments) {
        $result = & $script:realSymbolInvoker $Tool $Arguments
        if ($Arguments[0] -ceq '--strip-debug') {
            $layout = Get-LakeOwnPeLayout $Arguments[2]
            $section = @($layout.Sections | Where-Object { $_.Executable })[0]
            Set-SymbolByte $Arguments[2] $section.RawOffset
        }
        return $result
    }
    try { Reject-Symbol { Split-SymbolFixture $f } 'changed PE runtime field/code' }
    finally { Set-Item Function:Invoke-LakeSymbolObjcopy $script:realSymbolInvoker }
    Require-Symbol (!(Test-Path -LiteralPath $f.Output) -and !(Test-Path -LiteralPath $f.Symbols) -and
        (Get-LakeSymbolHash $f.Input) -ceq $hash) 'Bad strip promoted output or changed its input.'
}
Case-Symbol 'Reparse-point output ancestors fail closed without touching the target' {
    $f = New-SymbolFixture
    $target = Join-Path $f.Root 'owned-target'; [IO.Directory]::CreateDirectory($target) | Out-Null
    $link = Join-Path $f.Root 'owned-junction'
    New-Item -ItemType Junction -Path $link -Target $target -ErrorAction Stop | Out-Null
    Reject-Symbol { Split-LakeOwnSymbols $f.Input (Join-Path $link $f.Name) $f.Symbols $script:identity } 'Reparse points'
    Require-Symbol (!@(Get-ChildItem -LiteralPath $target -Force).Count) 'Junction target was modified.'
}
Case-Symbol 'All original own build artifacts remain unchanged; no PE has been run' {
    foreach ($path in $sourceHashes.Keys) { Require-Symbol ((Get-LakeSymbolHash $path) -ceq $sourceHashes[$path]) 'Original build artifact changed.' }
}

Write-Host "Symbols: $script:symbolPassed passed, $script:symbolFailed failed; BuildId=$script:identity; LLVM=$($script:tool.Version)."
Write-Host "Own fixture sandbox retained at $sandbox (ignored build files only). No input PE, GPU, Lake, hardware, installer or network was executed."
if ($script:symbolFailed) { throw 'Own symbols tests failed.' }
