<#
.SYNOPSIS
  Downloads an OBS Studio Windows x64 release and extracts the few files the build needs.
.DESCRIPTION
  Use this on machines without OBS installed, such as CI. It extracts obs64.exe (for its version), obs.dll and
  obs-frontend-api.dll from the official release zip into deps\obs-<version>. Import libraries are generated from
  those DLLs at configure time. Pass the folder to build.ps1 -ObsDir.
.EXAMPLE
  .\scripts\fetch-obs-binaries.ps1 -Version 33.0.3
  .\scripts\build.ps1 -ObsDir deps\obs-33.0.3
#>
param(
    [Parameter(Mandatory)][string]$Version,
    # Expected SHA-256 of the release zip (hex). Recommended in CI.
    [string]$Sha256,
    [string]$Dest
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Dest) { $Dest = Join-Path $root "deps\obs-$Version" }
$files = 'bin/64bit/obs64.exe', 'bin/64bit/obs.dll', 'bin/64bit/obs-frontend-api.dll'

if (-not ($files | Where-Object { -not (Test-Path (Join-Path $Dest $_)) })) {
    Write-Host "OBS $Version binaries already present in $Dest"
    return
}

$zipName = "OBS-Studio-$Version-Windows-x64.zip"
$url = "https://github.com/obsproject/obs-studio/releases/download/$Version/$zipName"
$tmp = Join-Path ([IO.Path]::GetTempPath()) "obs-indicator-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory $tmp | Out-Null
try {
    $zip = Join-Path $tmp $zipName
    Write-Host "Downloading $url"
    curl.exe -fsSL --retry 3 -o $zip $url
    if ($LASTEXITCODE) { throw "Download failed: $url" }

    if ($Sha256) {
        $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash
        if ($actual -ne $Sha256) { throw "SHA-256 mismatch for $zipName`n  expected $Sha256`n  actual   $actual" }
        Write-Host "SHA-256 verified"
    }

    New-Item -ItemType Directory -Force $Dest | Out-Null
    # Windows' bsdtar reads zip files (Git's GNU tar, which may come first on PATH, does not).
    & "$env:SystemRoot\System32\tar.exe" -xf $zip -C $Dest @files
    if ($LASTEXITCODE) { throw "Extracting $($files -join ', ') from $zipName failed" }
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
Write-Host "OBS $Version binaries extracted to $Dest"
