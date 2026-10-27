# Installs the newest dist\<version>\Centsible.vst3 (or the old dist\Centsible.vst3) into the system VST3 folder. Asks for admin rights itself.
# Close FL Studio (or any host that has loaded the plugin) first: Windows will not replace a loaded plugin.
$ErrorActionPreference = 'Stop'
$newest = Get-ChildItem (Join-Path $PSScriptRoot 'dist') -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path (Join-Path $_.FullName 'Centsible.vst3') } | Sort-Object { [version]($_.Name -replace '[^0-9.]', '') } -ErrorAction SilentlyContinue | Select-Object -Last 1
$src = if ($newest) { Join-Path $newest.FullName 'Centsible.vst3' } else { Join-Path $PSScriptRoot 'dist\Centsible.vst3' }
$dst = 'C:\Program Files\Common Files\VST3'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

$host_ = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(FL64|FL|reaper|Ableton Live.*|bitwig.*|Cubase.*|studio one.*)$' }
if ($host_) {
    Write-Host "Close these first, then run this again: $($host_.ProcessName -join ', ')" -ForegroundColor Yellow
    Read-Host 'Press Enter to exit'
    exit 1
}

Copy-Item $src $dst -Recurse -Force
$f = Get-Item (Join-Path $dst 'Centsible.vst3\Contents\x86_64-win\Centsible.vst3')
Write-Host "Installed: $($f.FullName)`n  $($f.Length) bytes, $($f.LastWriteTime)" -ForegroundColor Green
Read-Host 'Done. Reopen your DAW and rescan plugins. Press Enter to exit'
