#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryDirectory,
    [Parameter(Mandatory)][string]$IconPath
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LakePatchIconResourceProbe {
    [DllImport("shell32.dll", EntryPoint="ExtractIconExW", CharSet=CharSet.Unicode, ExactSpelling=true)]
    public static extern uint ExtractIconEx(string path, int index, [Out] IntPtr[] large, [Out] IntPtr[] small, uint count);
    [DllImport("user32.dll", EntryPoint="DestroyIcon", ExactSpelling=true)]
    public static extern bool DestroyIcon(IntPtr icon);
}
'@

$source = [IO.Path]::GetFullPath($IconPath)
$launcher = [IO.Path]::GetFullPath((Join-Path $BinaryDirectory 'LakeWindowedLauncher.exe'))
if (!(Test-Path -LiteralPath $source -PathType Leaf) -or !(Test-Path -LiteralPath $launcher -PathType Leaf)) {
    throw 'Patch icon test inputs are missing.'
}

$iconBytes = [IO.File]::ReadAllBytes($source)
if ($iconBytes.Length -lt 6 -or [BitConverter]::ToUInt16($iconBytes, 0) -ne 0 -or
    [BitConverter]::ToUInt16($iconBytes, 2) -ne 1 -or [BitConverter]::ToUInt16($iconBytes, 4) -lt 1) {
    throw 'The PATCHED icon is not a valid Windows ICO.'
}
$icon = [System.Drawing.Icon]::new($source)
try {
    if ($icon.Width -lt 16 -or $icon.Height -lt 16) { throw 'The PATCHED icon has no usable image size.' }
} finally { $icon.Dispose() }

$generator = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\scripts\New-PatchIcon.ps1'))
if (!$generator.Contains("DrawString('PATCHED'")) { throw 'The icon generator label must be spelled PATCHED.' }

$large = [IntPtr[]]::new(1)
$small = [IntPtr[]]::new(1)
$count = [LakePatchIconResourceProbe]::ExtractIconEx($launcher, 0, $large, $small, 1)
if ($count -eq 0 -or ($large[0] -eq [IntPtr]::Zero -and $small[0] -eq [IntPtr]::Zero)) {
    throw 'The launcher does not contain the PATCHED icon resource.'
}
if ($large[0] -ne [IntPtr]::Zero) { [void][LakePatchIconResourceProbe]::DestroyIcon($large[0]) }
if ($small[0] -ne [IntPtr]::Zero) { [void][LakePatchIconResourceProbe]::DestroyIcon($small[0]) }
Write-Host 'PATCHED icon is valid and embedded in LakeWindowedLauncher.exe.'
