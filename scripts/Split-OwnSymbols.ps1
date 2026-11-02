#Requires -Version 5.1
# Dot-source library ONLY. Import performs no I/O, tool execution, Git or network.
# Zig DWARF separation; never use this on proprietary images or an MSVC/PDB build.

function Assert-LakeSymbolBuildId([string]$BuildId) {
    $parsed = [guid]::Empty
    if ($BuildId -cnotmatch '^(?:[0-9a-fA-F]{32}|[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})$' -or
        ![guid]::TryParse($BuildId,[ref]$parsed) -or $parsed -eq [guid]::Empty) { throw 'Expected the exact, nonempty build GUID (N or D format).' }
}

function Assert-LakeSymbolOwnName([string]$Name) {
    if ($Name -cnotin @('LakeWindowedFix.dll','LakeWindowedLauncher.exe','LakeFixGraphicsCheck.exe') -and
        $Name -cnotmatch '^LakeWindowedFix-Setup-v[0-9]+\.[0-9]+(?:\.[0-9]+)?\.exe$') {
        throw 'Not an admitted own product filename. Proprietary/fixture/renamed foreign files are forbidden.'
    }
}

function Get-LakeSymbolSafePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'Empty symbols path.' }
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    # No UNC/device paths, ADS or mapped network drives. Never follow a junction.
    if ($full -notmatch '^[A-Za-z]:\\' -or $full.Substring(2).Contains(':') -or
        ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($full))).DriveType -eq [IO.DriveType]::Network) {
        throw 'Symbols operations require a local filesystem path without alternate streams.'
    }
    $cursor = $full
    while ($cursor) {
        try {
            $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse points are forbidden in symbols paths.' }
        } catch [System.Management.Automation.ItemNotFoundException] { }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $full
}

function Get-LakeSymbolFile([string]$Path,[long]$MaximumBytes = 536870912) {
    $full = Get-LakeSymbolSafePath $Path
    $file = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    if ($file.PSIsContainer -or $file.Length -lt 1 -or $file.Length -gt $MaximumBytes) { throw 'Expected a bounded, nonempty regular symbols file.' }
    return $file
}

function Get-LakeSymbolHash([string]$Path) {
    $file = Get-LakeSymbolFile $Path
    return (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Get-LakeSymbolBytesHash([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-LakeOwnPeLayout {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path,[string]$BuildId)
    if ($BuildId) { Assert-LakeSymbolBuildId $BuildId }
    $file = Get-LakeSymbolFile $Path
    $stream = [IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 256 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Not a PE image.' }
        $stream.Position = 60
        $pe = [long]$reader.ReadUInt32()
        if ($pe -lt 64 -or $pe -gt $stream.Length - 24) { throw 'Invalid PE header range.' }
        $stream.Position = $pe
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c) { throw 'Only own PE x86 (I386) is admitted.' }
        $count = $reader.ReadUInt16()
        $null = $reader.ReadUInt32() # COFF timestamp, not a provenance source.
        $symbols = [long]$reader.ReadUInt32()
        $symbolCount = [long]$reader.ReadUInt32()
        $optionalSize = $reader.ReadUInt16()
        $fileCharacteristics = $reader.ReadUInt16()
        $optional = $pe + 24
        $table = $optional + $optionalSize
        if ($count -lt 1 -or $count -gt 96 -or $optionalSize -lt 224 -or $optionalSize -gt 4096 -or
            $table + 40L*$count -gt $stream.Length -or !($fileCharacteristics -band 2)) { throw 'Invalid PE/section table.' }
        $stream.Position = $optional
        if ($reader.ReadUInt16() -ne 0x10b) { throw 'Expected PE32, not PE32+.' }
        $stream.Position = $optional + 16
        $entry = $reader.ReadUInt32()
        $baseCode = $reader.ReadUInt32(); $baseData = $reader.ReadUInt32(); $imageBase = $reader.ReadUInt32()
        $sectionAlignment = $reader.ReadUInt32(); $fileAlignment = $reader.ReadUInt32()
        $stream.Position = $optional + 56
        $imageSize = $reader.ReadUInt32(); $headerSize = $reader.ReadUInt32()
        if ($headerSize -lt $table + 40L*$count -or $headerSize -gt $stream.Length -or
            !$sectionAlignment -or !$fileAlignment -or !$imageSize) { throw 'Invalid image/header extent.' }
        $stream.Position = $optional + 68
        $subsystem = $reader.ReadUInt16(); $dllCharacteristics = $reader.ReadUInt16()
        $stackReserve = $reader.ReadUInt32(); $stackCommit = $reader.ReadUInt32()
        $heapReserve = $reader.ReadUInt32(); $heapCommit = $reader.ReadUInt32()
        $null = $reader.ReadUInt32()
        $directoryCount = $reader.ReadUInt32()
        if ($directoryCount -gt 16 -or 96L+8L*$directoryCount -gt $optionalSize) { throw 'Invalid PE data directory table.' }
        $directories = @()
        for ($i = 0; $i -lt $directoryCount; $i++) {
            $directories += [pscustomobject]@{Index=$i;Rva=$reader.ReadUInt32();Size=$reader.ReadUInt32()}
        }
        if ($directoryCount -gt 4 -and ($directories[4].Rva -or $directories[4].Size)) { throw 'Signed/certificate-bearing images must not be stripped by this library.' }
        $strings = 0L; $stringsSize = 0L
        if ($symbols) {
            $strings = $symbols + 18L*$symbolCount
            if ($strings -lt $headerSize -or $strings -gt $stream.Length - 4) { throw 'Invalid COFF symbol/string table range.' }
            $stream.Position = $strings
            $stringsSize = [long]$reader.ReadUInt32()
            if ($stringsSize -lt 4 -or $stringsSize -gt 67108864 -or $stringsSize -gt $stream.Length - $strings) { throw 'Invalid COFF string table extent.' }
        } elseif ($symbolCount) { throw 'COFF symbol count without symbol table.' }
        $sections = @(); $identity = $false; $entryFound = $false
        for ($i = 0; $i -lt $count; $i++) {
            $sectionOffset = $table + 40L*$i
            $stream.Position = $sectionOffset
            $rawName = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(8)).TrimEnd([char]0)
            $virtualSize = $reader.ReadUInt32(); $rva = $reader.ReadUInt32()
            $rawSize = $reader.ReadUInt32(); $rawOffset = $reader.ReadUInt32()
            $stream.Position = $sectionOffset + 36
            $characteristics = $reader.ReadUInt32()
            $name = $rawName
            if ($name.StartsWith('/')) {
                if ($name -cnotmatch '^/([0-9]+)$' -or !$strings) { throw 'Unsupported/missing COFF long section name.' }
                $offset = [long]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
                if ($offset -lt 4 -or $offset -ge $stringsSize) { throw 'COFF section name points outside the string table.' }
                $stream.Position = $strings + $offset
                $label = [Collections.Generic.List[byte]]::new()
                $terminated = $false
                for ($j=0; $j -lt 1024 -and $offset+$j -lt $stringsSize; $j++) {
                    $b = $reader.ReadByte()
                    if (!$b) { $terminated = $true; break }
                    if ($b -lt 33 -or $b -gt 126) { throw 'Invalid section name encoding.' }
                    $label.Add($b)
                }
                if (!$terminated) { throw 'Unterminated COFF section name.' }
                $name = [Text.Encoding]::ASCII.GetString($label.ToArray())
            }
            if ($name -cnotmatch '^\.[A-Za-z0-9_$][A-Za-z0-9_.$-]{0,126}$' -or
                @($sections | Where-Object { $_.Name -ceq $name }).Count) { throw 'Unsafe/duplicate PE section name.' }
            $extent = [Math]::Max([long]$virtualSize,[long]$rawSize)
            if ([long]$rva+$extent -gt $imageSize -or
                ($rawSize -and ($rawOffset -lt $headerSize -or [long]$rawOffset+$rawSize -gt $stream.Length))) { throw 'Section outside PE image/file.' }
            foreach ($previous in $sections) {
                if ($extent -and $previous.Extent -and [long]$rva -lt [long]$previous.Rva+$previous.Extent -and
                    [long]$previous.Rva -lt [long]$rva+$extent) { throw 'Overlapping section RVAs.' }
                if ($rawSize -and $previous.RawSize -and [long]$rawOffset -lt [long]$previous.RawOffset+$previous.RawSize -and
                    [long]$previous.RawOffset -lt [long]$rawOffset+$rawSize) { throw 'Overlapping section file ranges.' }
            }
            $debug = $name -cmatch '^\.(?:debug|zdebug)(?:_|$)'
            $executable = ($characteristics -band 0x20000000) -ne 0
            if ($debug -and $executable) { throw 'Executable section mislabeled as debug.' }
            if ($executable -and $entry -ge $rva -and [long]$entry -lt [long]$rva+$extent) { $entryFound = $true }
            $stream.Position = $rawOffset
            $bytes = $reader.ReadBytes([int]$rawSize)
            if ($bytes.Length -ne $rawSize) { throw 'Truncated section.' }
            if ($BuildId -and !$debug -and ($characteristics -band 0x40000000) -and $rawSize) {
                # Accept only a NUL-terminated runtime literal, not a path/DWARF
                # substring or the BuildId-fixture flavor. No LoadLibrary/execute.
                if ([Text.Encoding]::ASCII.GetString($bytes).Contains($BuildId+[char]0) -or
                    [Text.Encoding]::Unicode.GetString($bytes).Contains($BuildId+[char]0)) { $identity = $true }
            }
            $sections += [pscustomobject][ordered]@{Name=$name;RawName=$rawName;Rva=$rva;VirtualSize=$virtualSize
                RawSize=$rawSize;RawOffset=$rawOffset;Characteristics=$characteristics;Executable=$executable
                Debug=$debug;Sha256=(Get-LakeSymbolBytesHash $bytes);Extent=$extent;HeaderOffset=$sectionOffset}
        }
        if (!$entryFound) { throw 'Entry RVA is not within an executable section.' }
        if ($BuildId -and !$identity) { throw 'Exact build GUID is absent from runtime sections; foreign/fixture/stale input refused.' }
        $code = @($sections | Where-Object { $_.Executable } | ForEach-Object {
            [pscustomobject][ordered]@{Name=$_.Name;Rva=$_.Rva;VirtualSize=$_.VirtualSize;RawSize=$_.RawSize;Characteristics=$_.Characteristics;Sha256=$_.Sha256}
        })
        $codeHash = Get-LakeSymbolBytesHash ([Text.Encoding]::UTF8.GetBytes((ConvertTo-Json -InputObject $code -Depth 4 -Compress)))
        return [pscustomobject]@{Machine=332;Magic=267;ImageBase=$imageBase;EntryRva=$entry;BaseOfCode=$baseCode;BaseOfData=$baseData
            SectionAlignment=$sectionAlignment;FileAlignment=$fileAlignment;ImageSize=$imageSize;Subsystem=$subsystem
            FileCharacteristics=$fileCharacteristics;DllCharacteristics=$dllCharacteristics;StackReserve=$stackReserve
            StackCommit=$stackCommit;HeapReserve=$heapReserve;HeapCommit=$heapCommit;Directories=$directories
            Sections=$sections;CodeSections=$code;CodeSha256=$codeHash;Bytes=$file.Length;BuildIdPresent=$identity
            PeOffset=$pe;OptionalOffset=$optional;SectionTableOffset=$table;StringTableOffset=$strings;StringTableSize=$stringsSize}
    } finally { $reader.Dispose(); $stream.Dispose() }
}

function Assert-LakeSymbolEquivalent($Before,$After) {
    foreach ($field in @('Machine','Magic','ImageBase','EntryRva','BaseOfCode','BaseOfData','SectionAlignment','FileAlignment',
        'Subsystem','DllCharacteristics','StackReserve','StackCommit','HeapReserve','HeapCommit','CodeSha256')) {
        if ($Before.$field -cne $After.$field) { throw "Stripping changed PE runtime field/code: $field." }
    }
    # Only COFF debug/line/local-symbol stripped flags may change.
    if (($Before.FileCharacteristics -band 0xfdf3) -ne ($After.FileCharacteristics -band 0xfdf3)) { throw 'Stripping changed runtime COFF flags.' }
    $beforeSections = @($Before.Sections | Where-Object { !$_.Debug })
    $afterSections = @($After.Sections | Where-Object { !$_.Debug })
    if ($beforeSections.Count -ne $afterSections.Count) { throw 'Stripping removed/added a runtime section.' }
    for ($i=0; $i -lt $beforeSections.Count; $i++) {
        foreach ($field in @('Name','Rva','VirtualSize','RawSize','Characteristics','Sha256')) {
            if ($beforeSections[$i].$field -cne $afterSections[$i].$field) { throw "Stripping mutated a runtime section: $field." }
        }
    }
    if (@($Before.Directories).Count -ne @($After.Directories).Count) { throw 'Changed data directory count.' }
    for ($i=0; $i -lt @($Before.Directories).Count; $i++) {
        if ($i -eq 6) { continue } # Debug-directory entry is not a runtime RVA.
        if ($Before.Directories[$i].Rva -ne $After.Directories[$i].Rva -or $Before.Directories[$i].Size -ne $After.Directories[$i].Size) {
            throw 'Stripping changed a runtime data directory RVA/size.'
        }
    }
}

function Assert-LakeSymbolDebugPair($Before,$Debug) {
    foreach ($field in @('Machine','Magic','ImageBase','EntryRva')) {
        if ($Before.$field -ne $Debug.$field) { throw 'Debug sidecar belongs to a different image layout.' }
    }
    $left = @($Before.Sections | Where-Object { $_.Debug })
    $right = @($Debug.Sections | Where-Object { $_.Debug })
    if (@($Debug.CodeSections | Where-Object { $_.RawSize }).Count) { throw 'Debug sidecar still contains executable raw bytes, not an only-keep-debug result.' }
    if (!$left.Count -or $left.Count -ne $right.Count) { throw 'Missing/excess DWARF sections in debug sidecar.' }
    for ($i=0; $i -lt $left.Count; $i++) {
        foreach ($field in @('Name','Rva','VirtualSize','RawSize','Sha256')) {
            if ($left[$i].$field -cne $right[$i].$field) { throw 'Debug sidecar changed original DWARF section bytes/layout.' }
        }
    }
}

function ConvertTo-LakeSymbolArgument([string]$Value) {
    # Windows argv quoting, including spaces, quotes and trailing backslashes.
    if ($Value.Length -and $Value -notmatch '[\s"]') { return $Value }
    $result = [Text.StringBuilder]::new(); [void]$result.Append('"'); $slashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') { $slashes++; continue }
        if ($ch -eq '"') { [void]$result.Append(('\' * ($slashes*2+1))) }
        else { [void]$result.Append(('\' * $slashes)) }
        [void]$result.Append($ch); $slashes = 0
    }
    [void]$result.Append(('\' * ($slashes*2))); [void]$result.Append('"')
    return $result.ToString()
}

function Invoke-LakeSymbolObjcopy([string]$Tool,[string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Tool
    $start.Arguments = (@($Arguments | ForEach-Object { ConvertTo-LakeSymbolArgument $_ }) -join ' ')
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    try {
        if (!$process.Start()) { throw 'Could not start llvm-objcopy.' }
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(120000)) { $process.Kill(); $process.WaitForExit(); throw 'llvm-objcopy timed out.' }
        $out = $stdout.GetAwaiter().GetResult(); $err = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "llvm-objcopy failed (exit $($process.ExitCode)): $err" }
        return ($out+"`n"+$err)
    } finally { $process.Dispose() }
}

function Resolve-LakeSymbolObjcopy([string]$ObjcopyPath = 'auto') {
    $candidates = @()
    if ($ObjcopyPath -ieq 'auto') {
        $candidates += @(Get-Command llvm-objcopy.exe,llvm-objcopy -CommandType Application -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })
        if ($env:ProgramFiles) { $candidates += Join-Path $env:ProgramFiles 'LLVM\bin\llvm-objcopy.exe' }
        if ($env:ProgramW6432 -and $env:ProgramW6432 -ine $env:ProgramFiles) { $candidates += Join-Path $env:ProgramW6432 'LLVM\bin\llvm-objcopy.exe' }
        $candidates += Join-Path (Split-Path $PSScriptRoot -Parent) 'build-tools\llvm-symbols-20261020\llvm-objcopy.exe'
    } else { $candidates = @($ObjcopyPath) }
    foreach ($candidate in @($candidates | Select-Object -Unique)) {
        $candidateLock = $null
        try {
            $tool = Get-LakeSymbolFile $candidate
            if ($tool.Extension -ine '.exe') { throw 'Expected native llvm-objcopy.exe, not a script wrapper.' }
            $candidateLock = [IO.File]::Open($tool.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
            $text = Invoke-LakeSymbolObjcopy $tool.FullName @('--version')
            $version = [regex]::Match($text,'(?m)^\s*LLVM version ([0-9]+\.[0-9]+\.[0-9]+)(?:\s|$)').Groups[1].Value
            if (!$version -or $text -notmatch 'llvm-objcopy') { throw 'Tool does not identify as LLVM objcopy.' }
            return [pscustomobject]@{Path=$tool.FullName;Version=$version;Sha256=(Get-LakeSymbolHash $tool.FullName)}
        } catch {
            if ($ObjcopyPath -ine 'auto') { throw }
        } finally { if ($candidateLock) { $candidateLock.Dispose() } }
    }
    throw 'No usable local LLVM objcopy. Supply -ObjcopyPath; this library never downloads tools.'
}

function New-LakeSymbolStageFile([string]$Path) {
    $file = [IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $file.Dispose()
}

function Test-LakeOwnSymbolRecord {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RecordFile,[Parameter(Mandatory)][string]$OutputFile,
        [Parameter(Mandatory)][string]$BuildId)
    Assert-LakeSymbolBuildId $BuildId
    $recordPath = (Get-LakeSymbolFile $RecordFile 1048576).FullName
    $record = [IO.File]::ReadAllText($recordPath) | ConvertFrom-Json -ErrorAction Stop
    $fields = @('Schema','Kind','BuildId','FileName','OutputName','DebugName','PreSha256','PostSha256','DebugSha256',
        'PreBytes','PostBytes','DebugBytes','Machine','ImageBase','EntryRva','CodeSha256','CodeSections','ToolName','ToolVersion','ToolSha256')
    $actual = @($record.PSObject.Properties.Name)
    if ($actual.Count -ne $fields.Count -or @($actual | Where-Object { $_ -cnotin $fields }).Count -or
        $record.Schema -ne 1 -or $record.Kind -cne 'LakeOwnDwarfSymbols' -or $record.BuildId -cne $BuildId) { throw 'Invalid/mismatched own symbols record.' }
    Assert-LakeSymbolOwnName $record.FileName
    if ($record.DebugName -cne ($record.FileName+'.debug') -or $record.OutputName -cne [IO.Path]::GetFileName($OutputFile) -or
        [IO.Path]::GetFileName($recordPath) -cne ($record.FileName+'.symbols.json') -or $record.ToolName -cne 'llvm-objcopy' -or
        $record.ToolVersion -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw 'Unexpected symbols filenames/tool metadata.' }
    foreach ($field in @('PreSha256','PostSha256','DebugSha256','CodeSha256','ToolSha256')) {
        if ($record.$field -cnotmatch '^[a-f0-9]{64}$') { throw 'Invalid symbols hash.' }
    }
    $directory = Split-Path $recordPath -Parent
    $original = Get-LakeSymbolFile (Join-Path $directory $record.FileName)
    $debug = Get-LakeSymbolFile (Join-Path $directory $record.DebugName)
    $output = Get-LakeSymbolFile $OutputFile
    foreach ($pair in @(@($original,'Pre'),@($output,'Post'),@($debug,'Debug'))) {
        $prefix = $pair[1]
        if ((Get-LakeSymbolHash $pair[0].FullName) -cne $record.($prefix+'Sha256') -or
            $pair[0].Length -ne $record.($prefix+'Bytes')) { throw 'Symbol artifact changed after separation.' }
    }
    $before = Get-LakeOwnPeLayout $original.FullName -BuildId $BuildId
    $after = Get-LakeOwnPeLayout $output.FullName -BuildId $BuildId
    $sidecar = Get-LakeOwnPeLayout $debug.FullName
    Assert-LakeSymbolEquivalent $before $after
    Assert-LakeSymbolDebugPair $before $sidecar
    if (@($after.Sections | Where-Object { $_.Debug }).Count -or $after.Bytes -ge $before.Bytes) { throw 'Output is not a smaller, debug-stripped image.' }
    if ($record.Machine -ne $before.Machine -or $record.ImageBase -ne $before.ImageBase -or $record.EntryRva -ne $before.EntryRva -or
        $record.CodeSha256 -cne $before.CodeSha256 -or
        (ConvertTo-Json -InputObject @($record.CodeSections) -Depth 4 -Compress) -cne
        (ConvertTo-Json -InputObject @($before.CodeSections) -Depth 4 -Compress)) { throw 'Symbols record runtime/code metadata was altered.' }
    return $record
}

function Split-LakeOwnSymbols {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$InputFile,[Parameter(Mandatory)][string]$OutputFile,
        [Parameter(Mandatory)][string]$SymbolDirectory,[Parameter(Mandatory)][string]$BuildId,
        [string]$ObjcopyPath = 'auto')
    Assert-LakeSymbolBuildId $BuildId
    Assert-LakeSymbolOwnName ([IO.Path]::GetFileName($InputFile))
    $inputPath = Get-LakeSymbolSafePath $InputFile
    $outputPath = Get-LakeSymbolSafePath $OutputFile
    $symbolPath = Get-LakeSymbolSafePath $SymbolDirectory
    if ($inputPath -ieq $outputPath -or [IO.Path]::GetFileName($symbolPath) -cne $BuildId -or
        $outputPath.StartsWith($symbolPath.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Use a NEW output outside the symbols store, and SymbolDirectory ending in the exact BuildId.'
    }
    $name = [IO.Path]::GetFileName($inputPath)
    $archive = Join-Path $symbolPath $name
    $debugFile = Join-Path $symbolPath ($name+'.debug')
    $recordFile = Join-Path $symbolPath ($name+'.symbols.json')
    foreach ($destination in @($outputPath,$archive,$debugFile,$recordFile)) {
        $null = Get-LakeSymbolSafePath $destination
        if (Test-Path -LiteralPath $destination) { throw 'Existing output/symbol artifact preserved. Choose a fresh destination/build store.' }
    }
    $null = Get-LakeSymbolFile $inputPath
    $inputLock = [IO.File]::Open($inputPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $toolLock = $null
    try {
        $before = Get-LakeOwnPeLayout $inputPath -BuildId $BuildId
        if (!@($before.Sections | Where-Object { $_.Debug -and $_.RawSize }).Count) { throw 'No embedded DWARF sections to split. Do not use this for an MSVC/PDB-only image.' }
        $preHash = Get-LakeSymbolHash $inputPath
        $tool = Resolve-LakeSymbolObjcopy $ObjcopyPath
        $toolLock = [IO.File]::Open($tool.Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        if ((Get-LakeSymbolHash $tool.Path) -cne $tool.Sha256) { throw 'Objcopy changed during resolution.' }
        $parent = Split-Path $outputPath -Parent
        [IO.Directory]::CreateDirectory($parent) | Out-Null
        $null = Get-LakeSymbolSafePath $parent
        $stage = Join-Path $parent ('.lake-symbols-'+[guid]::NewGuid().ToString('N'))
        if (Test-Path -LiteralPath $stage) { throw 'Stage collision preserved.' }
        [IO.Directory]::CreateDirectory($stage) | Out-Null
        # Short internal names avoid spending PS 5.1/MAX_PATH budget twice on
        # long product filenames. Final store names still identify the artifact.
        $fullStage = Join-Path $stage 'full.pe'
        $debugStage = Join-Path $stage 'dwarf.debug'
        $stripStage = Join-Path $stage 'image.pe'
        $jsonStage = Join-Path $stage 'receipt.json'
        $copy = [IO.File]::Open($fullStage,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $inputLock.CopyTo($copy); $copy.Flush($true) } finally { $copy.Dispose() }
        foreach ($path in @($debugStage,$stripStage,$jsonStage)) { New-LakeSymbolStageFile $path }
        if ((Get-LakeSymbolHash $fullStage) -cne $preHash) { throw 'Unstripped copy is not the exact input.' }
        $null = Invoke-LakeSymbolObjcopy $tool.Path @('--only-keep-debug',$fullStage,$debugStage)
        $null = Invoke-LakeSymbolObjcopy $tool.Path @('--strip-debug',$fullStage,$stripStage)
        $after = Get-LakeOwnPeLayout $stripStage -BuildId $BuildId
        $sidecar = Get-LakeOwnPeLayout $debugStage
        Assert-LakeSymbolEquivalent $before $after
        Assert-LakeSymbolDebugPair $before $sidecar
        if (@($after.Sections | Where-Object { $_.Debug }).Count -or $after.Bytes -ge $before.Bytes) { throw 'Objcopy did not produce a smaller debug-stripped image.' }
        if ((Get-LakeSymbolHash $fullStage) -cne $preHash -or (Get-LakeSymbolHash $inputPath) -cne $preHash -or
            (Get-LakeSymbolHash $tool.Path) -cne $tool.Sha256) { throw 'Input/archive/tool changed during separation.' }
        $record = [pscustomobject][ordered]@{Schema=1;Kind='LakeOwnDwarfSymbols';BuildId=$BuildId;FileName=$name
            OutputName=[IO.Path]::GetFileName($outputPath);DebugName=($name+'.debug');PreSha256=$preHash
            PostSha256=(Get-LakeSymbolHash $stripStage);DebugSha256=(Get-LakeSymbolHash $debugStage)
            PreBytes=$before.Bytes;PostBytes=$after.Bytes;DebugBytes=$sidecar.Bytes;Machine=$before.Machine
            ImageBase=$before.ImageBase;EntryRva=$before.EntryRva;CodeSha256=$before.CodeSha256;CodeSections=@($before.CodeSections)
            ToolName='llvm-objcopy';ToolVersion=$tool.Version;ToolSha256=$tool.Sha256}
        [IO.File]::WriteAllText($jsonStage,($record | ConvertTo-Json -Depth 8)+"`n",[Text.UTF8Encoding]::new($false))
        # No overwriting rename. Archive + record first, stripped public image LAST.
        # A failure retains owned stage/partial archives for review, never rolls
        # back by deleting an unfamiliar file. No recursive cleanup on any path.
        [IO.Directory]::CreateDirectory($symbolPath) | Out-Null
        $null = Get-LakeSymbolSafePath $symbolPath
        foreach ($destination in @($archive,$debugFile,$recordFile,$outputPath)) {
            $null = Get-LakeSymbolSafePath $destination
            if (Test-Path -LiteralPath $destination) { throw 'Destination appeared during splitting; preserved.' }
        }
        [IO.File]::Move($fullStage,$archive)
        [IO.File]::Move($debugStage,$debugFile)
        [IO.File]::Move($jsonStage,$recordFile)
        [IO.File]::Move($stripStage,$outputPath)
        [IO.Directory]::Delete($stage) # Empty, nonrecursive, owned stage only.
        return (Test-LakeOwnSymbolRecord -RecordFile $recordFile -OutputFile $outputPath -BuildId $BuildId)
    } finally { if ($toolLock) { $toolLock.Dispose() }; $inputLock.Dispose() }
}
