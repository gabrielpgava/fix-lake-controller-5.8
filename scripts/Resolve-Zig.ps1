#Requires -Version 5.1
# No download, installation or PATH changes. This helper only selects an existing
# complete compiler; its version and resources must match the validated toolchain.
function Get-LakeZigPath {
    [CmdletBinding()]
    param([AllowEmptyString()][string]$Requested = 'auto')
    $project = Split-Path $PSScriptRoot -Parent
    $candidates = @()
    if ([string]::IsNullOrWhiteSpace($Requested) -or $Requested -eq 'auto') {
        $candidates += Join-Path $project 'build-tools\zig-0.14.1\zig-x86_64-windows-0.14.1\zig.exe'
        $candidates += Join-Path $project 'build-tools\python\ziglang\zig.exe'
        $command = Get-Command zig.exe -ErrorAction SilentlyContinue
        if ($command) { $candidates += $command.Source }
    } else {
        $command = Get-Command $Requested -ErrorAction Stop
        $candidates += $command.Source
    }
    foreach ($candidate in $candidates) {
        if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $directory = Split-Path $candidate -Parent
        $complete = $true
        foreach ($resource in @('lib\libc\mingw\crt\crtexe.c','lib\libcxx\src\algorithm.cpp',
                'lib\libunwind\src\libunwind.cpp','lib\std\zip\test.zig')) {
            if (!(Test-Path -LiteralPath (Join-Path $directory $resource) -PathType Leaf)) { $complete = $false; break }
        }
        if (!$complete) { continue }
        $version = @(& $candidate version)
        if ($LASTEXITCODE -eq 0 -and ($version -join '').Trim() -eq '0.14.1') { return [IO.Path]::GetFullPath($candidate) }
    }
    throw 'Zig 0.14.1 complete toolchain not found. Set ZIG=C:/tools/zig/zig.exe (or -ZigPath); nothing is downloaded automatically.'
}
