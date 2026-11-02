#Requires -Version 5.1
[CmdletBinding()]
param([string]$ZigPath='auto',[string]$BuildDirectory='build-graphics-check',[string]$ObjcopyPath='auto')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Resolve-Zig.ps1')
. (Join-Path $PSScriptRoot 'Build-Provenance.ps1')
. (Join-Path $PSScriptRoot 'Product-Metadata.ps1')
. (Join-Path $PSScriptRoot 'Preserve-OwnSymbols.ps1')
$zig=Get-LakeZigPath -Requested $ZigPath
$build=Assert-LakeSafePath $(if ([IO.Path]::IsPathRooted($BuildDirectory)) {$BuildDirectory} else {Join-Path $root $BuildDirectory})
New-Item -ItemType Directory -Path $build -Force | Out-Null
$source=Get-LakeSourceIdentity $root
$id=[guid]::NewGuid().ToString('N')
$stamp=Join-Path $build 'graphics_build_stamp.h'
[IO.File]::WriteAllText($stamp,"#define LAKE_FIX_BUILD_ID `"$id`"`n",[Text.Encoding]::ASCII)
$linked=Join-Path $build 'linked'
[IO.Directory]::CreateDirectory($linked) | Out-Null
$exe=Join-Path $build ('graphics-'+$id+'\LakeFixGraphicsCheck.exe')
$symbols=Join-Path $build ('symbols\'+$id)
$arguments=@('c++','-target','x86-windows-gnu','-std=c++17','-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0a00',
    '-O2','-g','-Wall','-Wextra','-Wpedantic','-include',$stamp,"-ffile-prefix-map=$root=.",
    (Join-Path $root 'src/graphics_check.cpp'),(Join-Path $root 'src/ddraw_scale.cpp'),(Join-Path $root 'src/profile.cpp'),
    '-luser32','-lgdi32','-lwtsapi32','-municode','-o',(Join-Path $linked 'LakeFixGraphicsCheck.exe'))
& $zig @arguments
if ($LASTEXITCODE) { throw 'Own graphics diagnostic compilation failed.' }
$null=Save-LakeOwnLinkSymbols -InputFile (Join-Path $linked 'LakeFixGraphicsCheck.exe') -OutputFile $exe `
    -SymbolDirectory $symbols -BuildId $id -Toolchain Zig -ObjcopyPath $ObjcopyPath
$tests=@()
foreach ($size in @(@(1088,612),@(320,180),@(4096,4096),@(320,4096),@(4096,180),@(701,503))) {
    $log=Join-Path $build ("self-test-$id-$($size[0])x$($size[1]).log")
    & $exe --self-test --width $size[0] --height $size[1] > $log
    if ($LASTEXITCODE) { throw "CPU self-test failed: $log" }
    $tests += Get-LakeFileRecord $log ([IO.Path]::GetFileName($log))
}
$confirmed=Get-LakeSourceIdentity $root
if ($source.Fingerprint -ne $confirmed.Fingerprint) { throw 'Source changed during graphics-check build; rebuild before using the report.' }
$null=Test-LakeOwnLinkSymbolRecord -RecordFile (Join-Path $symbols 'LakeFixGraphicsCheck.exe.link-symbols.json') -OutputFile $exe -BuildId $id
Copy-Item -LiteralPath $exe -Destination (Join-Path $build 'LakeFixGraphicsCheck.exe')
if ((Get-LakeSymbolHash $exe) -cne (Get-LakeSymbolHash (Join-Path $build 'LakeFixGraphicsCheck.exe'))) { throw 'Published diagnostic is not the exact CPU-tested image.' }
Write-LakeJson (Join-Path $build 'GRAPHICS-BUILD.json') ([ordered]@{Schema=1;Version=(Get-LakeProductMetadata).Version;BuildId=$id
    SourceCommit=$source.Commit;SourceDirty=$source.Dirty;SourceFingerprint=$source.Fingerprint;Toolchain='Zig 0.14.1';Architecture='x86'
    Arguments=$arguments;Binary=(Get-LakeFileRecord $exe 'LakeFixGraphicsCheck.exe');CpuTests=$tests;GraphicsStatus='NOT_EXECUTED';LakeStatus='NOT_EXECUTED'})
Copy-Item -LiteralPath (Join-Path $build 'GRAPHICS-BUILD.json') -Destination $symbols
Write-Host "Compiled own diagnostic and CPU-tested: $exe. GPU/desktop NOT EXECUTED; no Lake, installation or audio."
