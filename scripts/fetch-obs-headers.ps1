<#
.SYNOPSIS
  Fetches the libobs and frontend-API headers that match the installed OBS Studio.
.DESCRIPTION
  Sparse, shallow clone of obsproject/obs-studio at the tag of the installed
  OBS version (only libobs/ and frontend/api/ are checked out) into deps/obs-studio.
#>
param(
    [string]$ObsDir = "$env:ProgramFiles\obs-studio",
    [string]$Version
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'deps\obs-studio'

if (-not $Version) {
    $exe = Join-Path $ObsDir 'bin\64bit\obs64.exe'
    if (-not (Test-Path $exe)) { throw "OBS not found at '$ObsDir'. Pass -ObsDir or -Version." }
    $raw = (Get-Item $exe).VersionInfo.ProductVersion
    if ($raw -notmatch '^(\d+\.\d+\.\d+)') { throw "Unrecognised OBS version '$raw'. Pass -Version." }
    $Version = $Matches[1]
}

if (Test-Path (Join-Path $dest '.git')) {
    $current = (git -C $dest describe --tags --exact-match 2>$null)
    if ($current -eq $Version) {
        Write-Host "OBS $Version headers already present in $dest"
        exit 0
    }
    Write-Host "Replacing OBS $current headers with $Version"
    Remove-Item -Recurse -Force $dest
}

Write-Host "Fetching OBS $Version headers into $dest"
git clone --quiet --depth 1 --branch $Version --filter=blob:none --sparse https://github.com/obsproject/obs-studio $dest
if ($LASTEXITCODE) { throw "git clone failed" }
git -C $dest sparse-checkout set --no-cone /libobs/ /frontend/api/ /COPYING
if ($LASTEXITCODE) { throw "git sparse-checkout failed" }
