#Requires -Version 5.1
# Canonical product identity lives in the source header used by all components.
function Get-LakeProductMetadata {
    $projectRoot = Split-Path $PSScriptRoot -Parent
    $header = [IO.File]::ReadAllText((Join-Path $projectRoot 'src/product_version.h'))
    $version = [regex]::Match($header, '(?m)^#define LAKE_FIX_VERSION "(\d+\.\d+\.\d+)"$').Groups[1].Value
    $abi = [regex]::Match($header, '(?m)^#define LAKE_FIX_ABI_VERSION (\d+)$').Groups[1].Value
    $schema = [regex]::Match($header, '(?m)^#define LAKE_FIX_CONFIG_SCHEMA (\d+)$').Groups[1].Value
    if (!$version -or !$abi -or !$schema) { throw 'Invalid central product metadata.' }
    [pscustomobject]@{ Version=$version; Abi=[int]$abi; ConfigSchema=[int]$schema
        SetupName="LakeWindowedFix-Setup-v$version.exe" }
}

function Get-LakeSupportedProfile {
    $header = [IO.File]::ReadAllText((Join-Path (Split-Path $PSScriptRoot -Parent) 'src/lake_build_profile.h'))
    $exe = [regex]::Match($header, '(?m)^#define LAKE_BUILD007_EXE_SHA256 "([a-f0-9]{64})"$').Groups[1].Value
    $contour = [regex]::Match($header, '(?m)^#define LAKE_BUILD007_CONTOUR_SHA256 "([a-f0-9]{64})"$').Groups[1].Value
    if (!$exe -or !$contour) { throw 'Invalid admitted Build 007 profile.' }
    [pscustomobject]@{ Name='Lake Controller 5.8 Build 007'; Architecture='x86'; ExeSha256=$exe; ContourSha256=$contour }
}
