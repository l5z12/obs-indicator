<#
.SYNOPSIS
  Installs or updates obs-indicator from its latest GitHub release.
.EXAMPLE
  irm https://raw.githubusercontent.com/l5z12/obs-indicator/main/install.ps1 | iex
.EXAMPLE
  & ([scriptblock]::Create((irm https://raw.githubusercontent.com/l5z12/obs-indicator/main/install.ps1))) -Version 1.0.0
#>
# Everything runs inside a script block so that `irm | iex` leaves no variables or preferences behind in the
# caller's session. Keep this file ASCII: Windows PowerShell reads a saved copy as ANSI.
& {
    param(
        # A release such as 1.0.0, or 'latest'.
        [string]$Version = 'latest',
        # The plugin folder. OBS loads plugins from %ProgramData%\obs-studio\plugins.
        [string]$Destination = "$env:ProgramData\obs-studio\plugins\obs-indicator",
        # Install this local zip (made by scripts\build.ps1 -Package) instead of downloading a release.
        [string]$Zip
    )
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue' # Windows PowerShell downloads very slowly while drawing progress
    $repo = 'l5z12/obs-indicator'
    $minObs = 33

    if (-not [Environment]::Is64BitOperatingSystem -or [Environment]::OSVersion.Version.Build -lt 19041) {
        throw 'obs-indicator needs 64-bit Windows 10 (version 2004 or newer) or Windows 11.'
    }
    $obs = Get-ItemProperty 'HKLM:\SOFTWARE\OBS Studio' -ErrorAction SilentlyContinue
    if ($obs -and $obs.'(default)') {
        $exe = Join-Path $obs.'(default)' 'bin\64bit\obs64.exe'
        if (Test-Path -LiteralPath $exe) {
            $major = (Get-Item -LiteralPath $exe).VersionInfo.FileMajorPart
            if ($major -and $major -lt $minObs) {
                Write-Warning "OBS Studio $major is installed, but obs-indicator needs OBS $minObs or newer."
            }
        }
    }

    # An absolute path without a trailing separator, because the elevated copy below runs in another folder and
    # the swap puts its temporary folders next to this one.
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
    $full = [IO.Path]::GetFullPath($full)
    $Destination = $full.TrimEnd('\', '/')
    if ($Destination.Length -le [IO.Path]::GetPathRoot($full).TrimEnd('\', '/').Length) {
        throw "The plugin folder can't be a drive's root: $full"
    }

    # The DLLs in $Dir that a program has loaded (Windows can't replace those), or $null when only an
    # administrator could tell.
    $findBusy = {
        param([string]$Dir)
        $busy = @()
        foreach ($dll in @(Get-ChildItem -LiteralPath $Dir -Filter *.dll -ErrorAction SilentlyContinue)) {
            try { [IO.File]::Open($dll.FullName, 'Open', 'ReadWrite', 'None').Dispose() }
            catch [UnauthorizedAccessException] { return $null }
            catch [IO.IOException] { $busy += $dll.Name }
        }
        , $busy
    }

    # What to close so that the DLLs named in $Busy are free.
    $describe = {
        param([string[]]$Busy)
        $todo = @()
        if ($Busy -contains 'obs-indicator.dll' -or $Busy -contains 'WindowTopMost.dll') { $todo += 'close OBS' }
        elseif ($Busy -contains 'IAMKeyHacker.dll') {
            # Explorer keeps the WindowTopMost helper until OBS closes cleanly.
            $todo += 'close OBS (if it is closed, restart Windows Explorer in Task Manager)'
        }
        if ($Busy -contains 'obs-indicator-hook.dll') { $todo += 'close the game the overlay draws inside' }
        if (-not $todo) { $todo += "close the program that uses $($Busy -join ', ')" }
        $todo -join ' and '
    }

    # Swaps $From in for the plugin folder $To, so a failure leaves the old version in place. It also runs in an
    # elevated PowerShell, from its text and $findBusy's, so it uses nothing else.
    $swap = {
        param([string]$From, [string]$To)
        $ErrorActionPreference = 'Stop'
        $busy = & $findBusy $To
        if ($null -eq $busy) { throw [UnauthorizedAccessException]::new('Access is denied.') }
        if ($busy.Count) { throw "In use: $($busy -join ', ')" }

        $parent = Split-Path -Parent $To
        $leaf = Split-Path -Leaf $To
        New-Item -ItemType Directory -Force $parent | Out-Null
        # Leftovers of earlier runs. One that a program still has loaded waits for a later run.
        Get-ChildItem -LiteralPath $parent -Directory | Where-Object {
            $_.Name.StartsWith("$leaf.new-") -or $_.Name.StartsWith("$leaf.old-")
        } | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }

        $id = [guid]::NewGuid().ToString('N').Substring(0, 8)
        $new = Join-Path $parent "$leaf.new-$id"
        $old = Join-Path $parent "$leaf.old-$id"
        # Copy rather than move, so the files get the plugins folder's permissions instead of the temp folder's.
        try { Copy-Item -LiteralPath $From -Destination $new -Recurse }
        catch { Remove-Item -LiteralPath $new -Recurse -Force -ErrorAction SilentlyContinue; throw }
        if (Test-Path -LiteralPath $To) { Rename-Item -LiteralPath $To -NewName (Split-Path -Leaf $old) }
        try {
            Rename-Item -LiteralPath $new -NewName $leaf
        } catch {
            if (Test-Path -LiteralPath $old) { Rename-Item -LiteralPath $old -NewName $leaf }
            throw
        }
        if (Test-Path -LiteralPath $old) {
            try { Remove-Item -LiteralPath $old -Recurse -Force }
            catch { Write-Warning "Couldn't remove the old copy at $old yet; the next run removes it." }
        }
    }

    $tmp = Join-Path ([IO.Path]::GetTempPath()) "obs-indicator-install-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory $tmp | Out-Null
    # GitHub needs TLS 1.2, which old .NET settings leave out. SystemDefault (0) already includes it.
    $tls = [Net.ServicePointManager]::SecurityProtocol
    try {
        if ([int]$tls -ne 0) {
            [Net.ServicePointManager]::SecurityProtocol = $tls -bor [Net.SecurityProtocolType]::Tls12
        }
        if ($Zip) {
            $file = (Resolve-Path -LiteralPath $Zip).Path
            $label = Split-Path -Leaf $file
        } else {
            $tag = if ($Version -eq 'latest') { 'latest' } else { "tags/v$($Version.TrimStart('v'))" }
            try {
                $release = Invoke-RestMethod -UseBasicParsing "https://api.github.com/repos/$repo/releases/$tag"
            } catch {
                $status = [int]$_.Exception.Response.StatusCode
                $what = if ($Version -eq 'latest') { 'releases' } else { "release $Version" }
                if ($status -eq 404) { throw "$repo has no $what on GitHub." }
                if ($status -eq 403 -or $status -eq 429) {
                    throw 'GitHub is limiting requests from your network right now. Try again in an hour.'
                }
                throw "Couldn't reach GitHub: $($_.Exception.Message)"
            }
            $asset = $release.assets | Where-Object name -Match '^obs-indicator-[0-9.]+-windows-x64\.zip$' |
                Select-Object -First 1
            if (-not $asset) { throw "Release $($release.tag_name) has no Windows zip." }
            if ($asset.digest -notmatch '^sha256:([0-9a-f]{64})$') {
                throw "GitHub lists no SHA-256 for $($asset.name)."
            }
            $sha256 = $Matches[1]
            $label = "obs-indicator $($release.tag_name.TrimStart('v'))"

            Write-Host "Downloading $($asset.name)"
            $file = Join-Path $tmp $asset.name
            Invoke-WebRequest -UseBasicParsing $asset.browser_download_url -OutFile $file
            if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $sha256) {
                throw "$($asset.name) doesn't match the SHA-256 that GitHub lists for it, so it wasn't installed."
            }
        }

        # Not Expand-Archive: Windows PowerShell can pick up PowerShell 7's copy of it, which fails there.
        $unpacked = Join-Path $tmp 'unpacked'
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($file, $unpacked)
        $from = Join-Path $unpacked 'obs-indicator'
        if (-not (Test-Path -LiteralPath (Join-Path $from 'obs-indicator.dll'))) {
            throw "$label doesn't contain obs-indicator\obs-indicator.dll."
        }

        $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
        while ($true) {
            $waiting = $null
            while ($true) {
                $busy = & $findBusy $Destination
                # Without the rights to tell, wait for OBS, which is what usually has the plugin loaded.
                if ($null -eq $busy) {
                    $busy = @(if (Get-Process obs64 -ErrorAction SilentlyContinue) { 'obs-indicator.dll' })
                }
                if (-not $busy.Count) { break }
                $message = "Waiting for the installed copy to be free: $(& $describe $busy). Ctrl+C cancels."
                if ($message -ne $waiting) { Write-Host $message; $waiting = $message }
                Start-Sleep -Seconds 1
            }
            try {
                & $swap $from $Destination
                break
            } catch {
                if ($_.Exception.Message -like 'In use: *') { continue } # loaded again in the meantime
                # Only missing rights are worth a UAC prompt.
                $denied = $false
                for ($e = $_.Exception; $e; $e = $e.InnerException) {
                    if ($e -is [UnauthorizedAccessException] -or $e.HResult -eq -2147024891) { $denied = $true }
                }
                if ($admin -or -not $denied) { throw }
                $why = $_.Exception.Message.TrimEnd('.')
            }

            Write-Host "Couldn't write to $Destination ($why), asking for administrator rights"
            $errorFile = Join-Path $tmp 'error.txt'
            # Strings go in as Base64, so no character in a path can break the command's quoting.
            $text = { param($s)
                $b = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s))
                "([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('$b')))"
            }
            $command = "`$findBusy = { $findBusy }; try { & { $swap } $(& $text $from) $(& $text $Destination) } " +
                "catch { Set-Content -LiteralPath $(& $text $errorFile) -Encoding UTF8 -Value `$_.Exception.Message; " +
                "exit 1 }"
            $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
            # The console PowerShell of this edition, not the host program (which may be the ISE or an editor).
            $shellName = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' }
            $shell = Join-Path $PSHOME $shellName
            try {
                $p = Start-Process $shell -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
                    -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded
            } catch {
                throw "Not installed: writing to $Destination needs administrator rights, which weren't given."
            }
            if (Test-Path -LiteralPath $errorFile) {
                $why = (Get-Content -LiteralPath $errorFile -Raw -Encoding UTF8).Trim()
                if ($why -like 'In use: *') {
                    throw "Not installed: $(& $describe ($why.Substring(8) -split ', ')), then run this again."
                }
                throw "Installing as administrator failed: $why"
            }
            if ($p.ExitCode -or -not (Test-Path -LiteralPath (Join-Path $Destination 'obs-indicator.dll'))) {
                throw "Installing as administrator failed (exit code $($p.ExitCode))."
            }
            break
        }
    } finally {
        [Net.ServicePointManager]::SecurityProtocol = $tls
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host "Installed $label to $Destination."
    if (Get-Process obs64 -ErrorAction SilentlyContinue) {
        Write-Host 'Restart OBS to load it, then open Tools > Overlay Indicator.'
    } else {
        Write-Host 'Start OBS and open Tools > Overlay Indicator.'
    }
} @args
