<#
.SYNOPSIS
  Checks data/locale/*.ini against en-US.ini.
.DESCRIPTION
  Fails on unknown keys, %1/%2 placeholders that differ from English, unescaped quotes (OBS would cut the
  text there), a UTF-8 BOM or invalid UTF-8. Missing keys are only listed: OBS shows English for them.
#>
$ErrorActionPreference = 'Stop'
$dir = Join-Path (Split-Path -Parent $PSScriptRoot) 'data\locale'
$utf8 = [Text.UTF8Encoding]::new($false, $true) # throws on invalid UTF-8
$problems = [Collections.Generic.List[string]]::new()

function Read-Locale([IO.FileInfo]$file) {
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $problems.Add("$($file.Name): starts with a UTF-8 BOM")
    }
    $map = [ordered]@{}
    $n = 0
    foreach ($line in $utf8.GetString($bytes).TrimStart([char]0xFEFF) -split "`r?`n") {
        $n++
        if ($line -match '^\s*(#|$)') { continue }
        if ($line -notmatch '^([A-Za-z0-9_.]+)="(.*)"$') { $problems.Add("$($file.Name):${n}: not key=""value"""); continue }
        $key, $value = $Matches[1], $Matches[2]
        if ($value -match '(?<!\\)"') { $problems.Add("$($file.Name):${n}: unescaped "" in $key") }
        $map[$key] = $value
    }
    $map
}

function Get-Placeholders([string]$text) { ([regex]::Matches($text, '%\d') | ForEach-Object Value | Sort-Object) -join ' ' }

$en = Read-Locale (Get-Item (Join-Path $dir 'en-US.ini'))
foreach ($file in Get-ChildItem $dir -Filter *.ini | Where-Object Name -ne 'en-US.ini') {
    $tr = Read-Locale $file
    foreach ($key in $tr.Keys) {
        if (-not $en.Contains($key)) { $problems.Add("$($file.Name): unknown key $key"); continue }
        if ((Get-Placeholders $tr[$key]) -ne (Get-Placeholders $en[$key])) {
            $problems.Add("$($file.Name): $key has placeholders [$(Get-Placeholders $tr[$key])], English has [$(Get-Placeholders $en[$key])]")
        }
    }
    $missing = @($en.Keys | Where-Object { -not $tr.Contains($_) })
    if ($missing) { Write-Host "$($file.Name): $($missing.Count) untranslated, shown in English: $($missing -join ', ')" }
}

if ($problems.Count) {
    $problems | ForEach-Object { Write-Host $_ }
    throw "$($problems.Count) problem(s) in data/locale"
}
Write-Host "Translations OK ($((Get-ChildItem $dir -Filter *.ini).Count) languages)"
