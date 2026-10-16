<#
  GameIsolatorVFS one-shot installer.

  Run it INSIDE the game's bin\Win64_Shipping_Client folder (where steam_api64.dll is).
  It downloads steam_api64.vfs.dll (+ a vfsproxy.ini template if you don't have one) from
  GitHub, renames the real steam_api64.dll to steam_api64_orig.dll, and installs the proxy.

  Easiest (open PowerShell, cd to that folder, then):
    cd "E:\...\<Install>\bin\Win64_Shipping_Client"
    irm https://raw.githubusercontent.com/Whatevser/GameIsolatorVFS/main/install.ps1 | iex

  Or save this file there and run:
    powershell -ExecutionPolicy Bypass -File install.ps1
    powershell -ExecutionPolicy Bypass -File install.ps1 "E:\...\bin\Win64_Shipping_Client"
#>
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repo = 'https://raw.githubusercontent.com/Whatevser/GameIsolatorVFS/main'
$dir  = if ($args.Count -ge 1) { (Resolve-Path $args[0]).Path } else { (Get-Location).Path }

$tgt  = Join-Path $dir 'steam_api64.dll'
$orig = Join-Path $dir 'steam_api64_orig.dll'
$vfs  = Join-Path $dir 'steam_api64.vfs.dll'
$ini  = Join-Path $dir 'vfsproxy.ini'

if (-not (Test-Path $tgt) -and -not (Test-Path $orig)) {
    Write-Host "[X] No steam_api64.dll in $dir" -ForegroundColor Red
    Write-Host "    Run this inside the game's ...\bin\Win64_Shipping_Client\ folder (or pass it as an argument)."
    return
}

Write-Host "[*] Downloading steam_api64.vfs.dll ..."
Invoke-WebRequest "$repo/steam_api64.vfs.dll" -OutFile $vfs -UseBasicParsing

$freshIni = $false
if (-not (Test-Path $ini)) {
    Invoke-WebRequest "$repo/vfsproxy.ini" -OutFile $ini -UseBasicParsing
    $freshIni = $true
}

if (Test-Path $orig) {
    Write-Host "[=] steam_api64_orig.dll already present - refreshing the proxy."
} else {
    Rename-Item $tgt 'steam_api64_orig.dll'
    Write-Host "[+] renamed steam_api64.dll -> steam_api64_orig.dll"
}

Copy-Item $vfs $tgt -Force
Write-Host "[+] installed steam_api64.dll"

$stale = Join-Path $dir 'vfscore.dll'
if (Test-Path $stale) { Remove-Item $stale -Force; Write-Host "[+] removed obsolete vfscore.dll" }

if ($freshIni) {
    Write-Host "[!] A template vfsproxy.ini was created - EDIT the real->virtual paths before launching." -ForegroundColor Yellow
    try { Start-Process notepad.exe $ini } catch {}
} else {
    Write-Host "[i] Using your existing vfsproxy.ini - make sure its paths are right."
}

Write-Host "[OK] Done. Launch the game; check vfsproxy.log for 'attach ... hooks enabled'." -ForegroundColor Green
