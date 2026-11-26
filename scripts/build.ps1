<#
.SYNOPSIS
  Builds obs-indicator, and optionally packages and/or installs it.
.EXAMPLE
  .\scripts\build.ps1                        # build into .\build
  .\scripts\build.ps1 -Install               # ...and install to %ProgramData%\obs-studio\plugins\obs-indicator
  .\scripts\build.ps1 -Package               # ...and write build\dist\obs-indicator-<version>-windows-x64.zip
  .\scripts\build.ps1 -ObsDir deps\obs-33.0.3 -Package -WarningsAsErrors    # what CI runs
#>
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$Config = 'RelWithDebInfo',
    # An OBS Studio install, or a folder made by fetch-obs-binaries.ps1.
    [string]$ObsDir = "$env:ProgramFiles\obs-studio",
    [switch]$Install,
    [string]$InstallDir = "$env:ProgramData\obs-studio\plugins\obs-indicator",
    [switch]$Package,
    [switch]$WarningsAsErrors
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
if (-not (Test-Path $ObsDir)) { throw "OBS not found at '$ObsDir'. Pass -ObsDir, or run fetch-obs-binaries.ps1." }
$ObsDir = (Resolve-Path $ObsDir).Path

# Headers matching that OBS version (does nothing if they are already checked out).
& (Join-Path $PSScriptRoot 'fetch-obs-headers.ps1') -ObsDir $ObsDir

# Enter an x64 MSVC developer environment (newest Visual Studio with the C++ tools).
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with "Desktop development with C++" is required.' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

$werror = if ($WarningsAsErrors) { 'ON' } else { 'OFF' }
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DOBS_INSTALL_DIR=$ObsDir" "-DCMAKE_COMPILE_WARNING_AS_ERROR=$werror"
if ($LASTEXITCODE) { throw 'CMake configure failed' }
cmake --build $build
if ($LASTEXITCODE) { throw 'Build failed' }

if ($Package) {
    $version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(obs-indicator VERSION ([0-9.]+)').Groups[1].Value
    $name = "obs-indicator-$version-windows-x64"
    $stage = Join-Path $build 'package'
    $dist = Join-Path $build 'dist'
    Remove-Item -Recurse -Force $stage, $dist -ErrorAction SilentlyContinue
    cmake --install $build --prefix (Join-Path $stage 'obs-indicator')
    if ($LASTEXITCODE) { throw 'Packaging failed' }

    # The plugin zip extracts into %ProgramData%\obs-studio\plugins; debug symbols go in a separate zip.
    New-Item -ItemType Directory -Force $dist, (Join-Path $stage 'symbols') | Out-Null
    Get-ChildItem (Join-Path $stage 'obs-indicator') -Filter *.pdb | Move-Item -Destination (Join-Path $stage 'symbols')
    $tar = "$env:SystemRoot\System32\tar.exe"
    & $tar -a -cf (Join-Path $dist "$name.zip") -C $stage obs-indicator
    if ($LASTEXITCODE) { throw 'Creating the plugin zip failed' }
    $pdbs = @(Get-ChildItem (Join-Path $stage 'symbols') | ForEach-Object Name)
    if ($pdbs) {
        & $tar -a -cf (Join-Path $dist "$name-pdb.zip") -C (Join-Path $stage 'symbols') @pdbs
        if ($LASTEXITCODE) { throw 'Creating the symbols zip failed' }
    }
    Get-ChildItem $dist | ForEach-Object { Write-Host "Packaged $($_.FullName)" }
}

if ($Install) {
    if (Get-Process obs64 -ErrorAction SilentlyContinue) {
        throw 'OBS is running; close it first so the plugin DLL can be replaced.'
    }
    # A game the overlay draws inside keeps obs-indicator-hook.dll loaded (and locked) until it exits.
    $hook = Join-Path $InstallDir 'obs-indicator-hook.dll'
    if (Test-Path $hook) {
        try { [IO.File]::Open($hook, 'Open', 'ReadWrite', 'None').Dispose() }
        catch { throw "$hook is in use; close the game the overlay draws inside first." }
    }
    # Remove a copy installed in the pre-OBS-33 layout (bin\64bit), which OBS would flag as Legacy.
    $legacy = Join-Path $InstallDir 'bin'
    if (Test-Path (Join-Path $legacy '64bit\obs-indicator.dll')) {
        Remove-Item -Recurse -Force $legacy
        Write-Host "Removed legacy layout $legacy"
    }
    cmake --install $build --prefix $InstallDir
    if ($LASTEXITCODE) { throw 'Install failed' }
    Write-Host "Installed to $InstallDir - restart OBS and open Tools > Overlay Indicator."
}
