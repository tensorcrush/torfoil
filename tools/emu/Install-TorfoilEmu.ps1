param(
    [Parameter(Mandatory = $true)][string]$Firmware,
    [Parameter(Mandatory = $true)][string]$Keys,
    [string]$EmuHome = "$env:LOCALAPPDATA\torfoil-emu",
    [string]$Version = '2.0.5',
    [string]$Url = ''
)

$ErrorActionPreference = 'Stop'

if (-not $Url) {
    $Url = "https://git.ryujinx.app/projects/Kenji-NX/releases/download/$Version/kenjinx-$Version-win_x64.zip"
}

New-Item -ItemType Directory -Force -Path $EmuHome, "$EmuHome\data\system", "$EmuHome\data\sdcard" | Out-Null

$zip = Join-Path $EmuHome 'emulator.zip'
if (-not (Test-Path "$EmuHome\app\Ryujinx.exe")) {
    Write-Output "telechargement de l'emulateur ($Version)"
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
    Write-Output "sha256 : $((Get-FileHash $zip -Algorithm SHA256).Hash)"
    Expand-Archive -Path $zip -DestinationPath "$EmuHome\app" -Force
    Remove-Item $zip -Force
}
Write-Output "emulateur : $EmuHome\app\Ryujinx.exe"

foreach ($k in 'prod.keys', 'title.keys') {
    $src = Join-Path $Keys $k
    if (Test-Path $src) { Copy-Item $src "$EmuHome\data\system\$k" -Force; Write-Output "cle : $k" }
}
if (-not (Test-Path "$EmuHome\data\system\prod.keys")) { throw "prod.keys introuvable dans $Keys" }

python "$PSScriptRoot\install_firmware.py" $Firmware "$EmuHome\data"

New-Item -ItemType Directory -Force -Path "$EmuHome\data\profiles\keyboard" | Out-Null
Copy-Item "$PSScriptRoot\profile-torfoil.json" "$EmuHome\data\profiles\keyboard\torfoil.json" -Force

Write-Output ''
Write-Output "pret. Essai : powershell -File tools\emu\Invoke-TorfoilEmu.ps1 -Script 'shot,quit'"
