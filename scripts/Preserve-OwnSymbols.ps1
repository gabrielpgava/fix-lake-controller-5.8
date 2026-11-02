#Requires -Version 5.1
# Import only defines functions. Never loads or executes a product PE/PDB.
. (Join-Path $PSScriptRoot 'Split-OwnSymbols.ps1')

function Get-LakeOwnCodeView([string]$Path,[string]$BuildId) {
    $layout = Get-LakeOwnPeLayout $Path -BuildId $BuildId
    $directory = @($layout.Directories | Where-Object { $_.Index -eq 6 })
    if (!$directory.Count -or !$directory[0].Size) { return $null }
    $entry = $directory[0]
    if ($entry.Size % 28 -or $entry.Size -gt 1792) { throw 'Unsupported PE debug directory extent.' }
    $section = @($layout.Sections | Where-Object {
        $entry.Rva -ge $_.Rva -and [long]$entry.Rva+$entry.Size -le [long]$_.Rva+$_.RawSize
    })
    if ($section.Count -ne 1) { throw 'PE debug directory is outside raw section bytes.' }
    $bytes = [IO.File]::ReadAllBytes((Get-LakeSymbolFile $Path).FullName)
    $offset = [long]$section[0].RawOffset+$entry.Rva-$section[0].Rva
    $result = $null
    for ($i=0; $i -lt $entry.Size; $i+=28) {
        if ([BitConverter]::ToUInt32($bytes,$offset+$i+12) -ne 2) { continue }
        if ($null -ne $result) { throw 'Multiple CodeView records are not supported.' }
        $length = [BitConverter]::ToUInt32($bytes,$offset+$i+16)
        $raw = [BitConverter]::ToUInt32($bytes,$offset+$i+24)
        if ($length -lt 25 -or $length -gt 32768 -or [long]$raw+$length -gt $bytes.Length -or
            [Text.Encoding]::ASCII.GetString($bytes,$raw,4) -cne 'RSDS') { throw 'Invalid/unsupported CodeView RSDS record.' }
        $guidBytes = [byte[]]::new(16); [Array]::Copy($bytes,$raw+4,$guidBytes,0,16)
        $label = [Text.Encoding]::UTF8.GetString($bytes,$raw+24,$length-24)
        $end = $label.IndexOf([char]0)
        if ($end -lt 1) { throw 'Unterminated CodeView PDB name.' }
        $name = [IO.Path]::GetFileName($label.Substring(0,$end))
        if ($name -cne ([IO.Path]::GetFileNameWithoutExtension($Path)+'.pdb')) { throw 'Unexpected own CodeView PDB basename.' }
        $result = [pscustomobject][ordered]@{Guid=([guid]::new($guidBytes)).ToString('N');Age=[BitConverter]::ToUInt32($bytes,$raw+20);Name=$name}
    }
    return $result
}

function Get-LakePdbIdentity([string]$Path) {
    $file = Get-LakeSymbolFile $Path
    $stream = [IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $magic = "Microsoft C/C++ MSF 7.00`r`n"+[char]26+"DS"+[char]0+[char]0+[char]0
        if ($stream.Length -lt 56 -or [Text.Encoding]::ASCII.GetString($reader.ReadBytes(32)) -cne $magic) { throw 'Expected an MSF 7.00 PDB from the same link.' }
        $blockSize = [long]$reader.ReadUInt32(); $null = $reader.ReadUInt32()
        $blocks = [long]$reader.ReadUInt32(); $directoryBytes = [long]$reader.ReadUInt32()
        $null = $reader.ReadUInt32(); $map = [long]$reader.ReadUInt32()
        if ($blockSize -notin @(512,1024,2048,4096,8192,16384,32768,65536) -or $blocks -lt 3 -or
            $blocks*$blockSize -ne $stream.Length -or $map -ge $blocks -or
            $directoryBytes -lt 12 -or $directoryBytes -gt 16777216) { throw 'Invalid PDB superblock/range.' }
        $directoryBlocks = [long][Math]::Ceiling($directoryBytes/[double]$blockSize)
        if ($directoryBlocks*4 -gt $blockSize) { throw 'PDB directory block map exceeds supported bounded extent.' }
        $stream.Position = $map*$blockSize
        $indices = @(); for ($i=0; $i -lt $directoryBlocks; $i++) { $indices += [long]$reader.ReadUInt32() }
        $directory = [byte[]]::new($directoryBytes); $copied = 0L
        foreach ($index in $indices) {
            if ($index -ge $blocks) { throw 'PDB directory block is outside the file.' }
            $stream.Position = $index*$blockSize
            $size = [int][Math]::Min($blockSize,$directoryBytes-$copied)
            $data = $reader.ReadBytes($size)
            if ($data.Length -ne $size) { throw 'Truncated PDB directory.' }
            [Array]::Copy($data,0,$directory,$copied,$size); $copied += $size
        }
        $count = [long][BitConverter]::ToUInt32($directory,0)
        if ($count -lt 2 -or $count -gt 65536 -or 4+$count*4 -gt $directoryBytes) { throw 'Invalid PDB stream inventory.' }
        $cursor = 4+$count*4; $info = $null
        for ($i=0; $i -lt $count; $i++) {
            $size = [long][BitConverter]::ToUInt32($directory,4+4*$i)
            if ($size -eq 4294967295) { continue }
            $pages = [long][Math]::Ceiling($size/[double]$blockSize)
            if ($size -gt $stream.Length -or $cursor+$pages*4 -gt $directoryBytes) { throw 'PDB stream exceeds directory/file bounds.' }
            if ($i -eq 1) {
                if ($size -lt 28 -or $size -gt 1048576) { throw 'Invalid PDB info stream size.' }
                $info = [byte[]]::new($size)
            }
            for ($j=0; $j -lt $pages; $j++) {
                $index = [long][BitConverter]::ToUInt32($directory,$cursor); $cursor += 4
                if ($index -ge $blocks) { throw 'PDB stream block is outside the file.' }
                if ($i -eq 1) {
                    $stream.Position = $index*$blockSize
                    $amount = [int][Math]::Min($blockSize,$size-$j*$blockSize)
                    $data = $reader.ReadBytes($amount)
                    if ($data.Length -ne $amount) { throw 'Truncated PDB info stream.' }
                    [Array]::Copy($data,0,$info,$j*$blockSize,$amount)
                }
            }
        }
        if ($null -eq $info) { throw 'PDB info stream is missing.' }
        $guidBytes = [byte[]]::new(16); [Array]::Copy($info,12,$guidBytes,0,16)
        return [pscustomobject][ordered]@{Guid=([guid]::new($guidBytes)).ToString('N');Age=[BitConverter]::ToUInt32($info,8);Name=$file.Name}
    } finally { $reader.Dispose(); $stream.Dispose() }
}

function Get-LakeLinkSymbolFileRecord([string]$Path) {
    $file = Get-LakeSymbolFile $Path
    [pscustomobject][ordered]@{Name=$file.Name;Sha256=(Get-LakeSymbolHash $file.FullName);Bytes=$file.Length}
}

function Assert-LakeLinkSymbolFileRecord($Record,[string]$Directory) {
    if ($null -eq $Record -or (@($Record.PSObject.Properties.Name) -join '|') -cne 'Name|Sha256|Bytes' -or
        $Record.Name -cne [IO.Path]::GetFileName($Record.Name) -or $Record.Sha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'Invalid private link symbol file record.' }
    $actual = Get-LakeLinkSymbolFileRecord (Join-Path $Directory $Record.Name)
    if ($actual.Sha256 -cne $Record.Sha256 -or $actual.Bytes -ne $Record.Bytes) { throw 'Private link symbols changed after preservation.' }
}

function Test-LakeOwnLinkSymbolRecord {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RecordFile,[Parameter(Mandatory)][string]$OutputFile,[Parameter(Mandatory)][string]$BuildId)
    Assert-LakeSymbolBuildId $BuildId
    $recordPath = (Get-LakeSymbolFile $RecordFile 1048576).FullName
    $record = [IO.File]::ReadAllText($recordPath) | ConvertFrom-Json -ErrorAction Stop
    if ((@($record.PSObject.Properties.Name) -join '|') -cne 'Schema|Kind|BuildId|Toolchain|Original|Final|Pdb|CodeView|Split' -or
        $record.Schema -ne 1 -or $record.Kind -cne 'LakeOwnLinkSymbols' -or $record.BuildId -cne $BuildId -or
        $record.Toolchain -cnotin @('Zig','MinGW','MSVC')) { throw 'Invalid private link symbols record.' }
    $store = Split-Path $recordPath -Parent
    Assert-LakeSymbolOwnName $record.Original.Name
    if ([IO.Path]::GetFileName($store) -cne $BuildId -or
        [IO.Path]::GetFileName($recordPath) -cne ($record.Original.Name+'.link-symbols.json') -or
        $record.Final.Name -cne [IO.Path]::GetFileName($OutputFile)) { throw 'Mismatched private link symbol filenames/build store.' }
    Assert-LakeLinkSymbolFileRecord $record.Original $store
    Assert-LakeLinkSymbolFileRecord $record.Final (Split-Path (Get-LakeSymbolSafePath $OutputFile) -Parent)
    $original = Join-Path $store $record.Original.Name
    $cv = Get-LakeOwnCodeView $original $BuildId
    if ((ConvertTo-Json -InputObject $cv -Compress) -cne (ConvertTo-Json -InputObject $record.CodeView -Compress)) { throw 'CodeView identity changed.' }
    if ($record.Split) {
        if ($record.Split.Name -cne ($record.Original.Name+'.symbols.json')) { throw 'Unexpected split receipt name.' }
        Assert-LakeLinkSymbolFileRecord $record.Split $store
        $null = Test-LakeOwnSymbolRecord -RecordFile (Join-Path $store $record.Split.Name) -OutputFile $OutputFile -BuildId $BuildId
    } elseif ($record.Toolchain -cne 'MSVC' -or $record.Original.Sha256 -cne $record.Final.Sha256) { throw 'Only an unchanged PDB-only MSVC image may omit the DWARF split.' }
    if ($record.Pdb) {
        if (!$cv -or $record.Pdb.Name -cne $cv.Name) { throw 'PDB has no matching CodeView record.' }
        Assert-LakeLinkSymbolFileRecord $record.Pdb $store
        $pdb = Get-LakePdbIdentity (Join-Path $store $record.Pdb.Name)
        if ($pdb.Guid -cne $cv.Guid -or $pdb.Age -ne $cv.Age) { throw 'PDB GUID/age does not match the exact link.' }
    } elseif ($record.Toolchain -cin @('Zig','MSVC') -or $null -ne $cv) { throw 'Exact same-link PDB is missing.' }
    return $record
}

function Save-LakeOwnLinkSymbols {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$InputFile,[Parameter(Mandatory)][string]$OutputFile,
        [Parameter(Mandatory)][string]$SymbolDirectory,[Parameter(Mandatory)][string]$BuildId,
        [Parameter(Mandatory)][ValidateSet('Zig','MSVC','MinGW')][string]$Toolchain,[string]$ObjcopyPath='auto')
    Assert-LakeSymbolBuildId $BuildId
    Assert-LakeSymbolOwnName ([IO.Path]::GetFileName($InputFile))
    $inputPath = (Get-LakeSymbolFile $InputFile).FullName
    $output = Get-LakeSymbolSafePath $OutputFile
    $store = Get-LakeSymbolSafePath $SymbolDirectory
    if ([IO.Path]::GetFileName($store) -cne $BuildId -or $inputPath -ieq $output -or
        $output.StartsWith($store.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Use a fresh output outside the exact BuildId store.' }
    $name = [IO.Path]::GetFileName($inputPath)
    $archive = Join-Path $store $name
    $receipt = Join-Path $store ($name+'.link-symbols.json')
    foreach ($path in @($archive,$output,$receipt)) { if (Test-Path -LiteralPath $path) { throw 'Existing link symbols/output preserved; use a new build destination.' } }
    $imageLock = [IO.File]::Open($inputPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $pdbLock = $null
    try {
        $cv = Get-LakeOwnCodeView $inputPath $BuildId
        $pdbRecord = $null
        if ($cv) {
            $pdbPath = (Get-LakeSymbolFile (Join-Path (Split-Path $inputPath -Parent) $cv.Name)).FullName
            $pdbLock = [IO.File]::Open($pdbPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
            $pdb = Get-LakePdbIdentity $pdbPath
            if ($pdb.Guid -cne $cv.Guid -or $pdb.Age -ne $cv.Age) { throw 'Adjacent PDB is not from this exact link.' }
            $pdbRecord = Get-LakeLinkSymbolFileRecord $pdbPath
            $pdbDestination = Join-Path $store $cv.Name
            if (Test-Path -LiteralPath $pdbDestination) { throw 'Existing PDB preserved.' }
        } elseif ($Toolchain -cin @('Zig','MSVC')) { throw 'Expected CodeView and same-link PDB for Zig/MSVC.' }
        [IO.Directory]::CreateDirectory($store) | Out-Null
        $split = $null
        if ($Toolchain -eq 'MSVC') {
            [IO.Directory]::CreateDirectory((Split-Path $output -Parent)) | Out-Null
            [IO.File]::Copy($inputPath,$archive,$false)
            [IO.File]::Copy($inputPath,$output,$false)
        } else {
            $null = Split-LakeOwnSymbols -InputFile $inputPath -OutputFile $output -SymbolDirectory $store -BuildId $BuildId -ObjcopyPath $ObjcopyPath
            $split = Get-LakeLinkSymbolFileRecord (Join-Path $store ($name+'.symbols.json'))
        }
        if ($pdbRecord) { [IO.File]::Copy($pdbPath,$pdbDestination,$false) }
        $record = [pscustomobject][ordered]@{Schema=1;Kind='LakeOwnLinkSymbols';BuildId=$BuildId;Toolchain=$Toolchain
            Original=(Get-LakeLinkSymbolFileRecord $archive);Final=(Get-LakeLinkSymbolFileRecord $output)
            Pdb=$pdbRecord;CodeView=$cv;Split=$split}
        $sink = [IO.File]::Open($receipt,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($record | ConvertTo-Json -Depth 8)+"`n"); $sink.Write($bytes,0,$bytes.Length); $sink.Flush($true) }
        finally { $sink.Dispose() }
        return (Test-LakeOwnLinkSymbolRecord -RecordFile $receipt -OutputFile $output -BuildId $BuildId)
    } finally { if ($pdbLock) { $pdbLock.Dispose() }; $imageLock.Dispose() }
}
