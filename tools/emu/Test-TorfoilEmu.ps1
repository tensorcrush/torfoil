param(
    [string]$Out = "$PSScriptRoot\..\..\emu-out",
    [string]$Nro = "$PSScriptRoot\..\..\torfoil.nro"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path

$seed = Join-Path ([System.IO.Path]::GetTempPath()) ("torfoil-seed-" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path "$seed\torfoil\downloads", "$seed\torfoil\inbox" | Out-Null
python "$root\host\mkfixtures.py" "$seed\torfoil\downloads" "$seed\torfoil\inbox" | Out-Null
Set-Content "$seed\torfoil\settings.cfg" -Encoding ASCII -Value @('language=fr','require_vpn=0','https_trackers_only=0','enable_dht=1','enable_pex=1','no_upload=0','max_active=2')

$script:fail = 0
function Check([string]$desc, [scriptblock]$test) {
    $ok = $false
    try { $ok = [bool](& $test) } catch { $ok = $false }
    if ($ok) { Write-Host "  OK    $desc" -ForegroundColor Green }
    else { Write-Host "  ECHEC $desc" -ForegroundColor Red; $script:fail++ }
}

function Variance([string]$png) {
    Add-Type -AssemblyName System.Drawing
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $sum = 0.0; $sum2 = 0.0; $n = 0
        for ($y = 0; $y -lt $bmp.Height; $y += 8) {
            for ($x = 0; $x -lt $bmp.Width; $x += 8) {
                $p = $bmp.GetPixel($x, $y)
                $v = ($p.R + $p.G + $p.B) / 3.0
                $sum += $v; $sum2 += $v * $v; $n++
            }
        }
        $mean = $sum / $n
        return [math]::Sqrt(($sum2 / $n) - ($mean * $mean))
    } finally { $bmp.Dispose() }
}

Write-Host '=== lancement'
& "$PSScriptRoot\Invoke-TorfoilEmu.ps1" -Nro $Nro -Sd $seed -Out $Out `
    -Script 'wait 4000,shot,A!,wait 1500,shot,B!,wait 1000,R!,wait 2000,shot,R!,R!,R!,wait 1500,shot,quit'

$sd = Join-Path $Out 'sdcard\torfoil'
$log = Join-Path $sd 'torfoil.log'
$logText = if (Test-Path $log) { Get-Content $log -Raw -Encoding UTF8 } else { '' }
$ready = ([regex]::Matches($logText, 'stockage prêt')).Count
$resume = @(Get-ChildItem "$sd\downloads" -Filter '.*.torfoil' -Force -ErrorAction SilentlyContinue).Count
$shots = @(Get-ChildItem "$Out\shot-*.png" -ErrorAction SilentlyContinue)
$blank = @($shots | Where-Object { (Variance $_.FullName) -lt 5 }).Count
$inbox = @(Get-ChildItem "$sd\inbox" -Force -ErrorAction SilentlyContinue).Count

Write-Host '=== verifications'
Check "le guest a ecrit torfoil.log"                     { Test-Path $log }
Check "l'interface a reagi aux touches (menu Actions)"  { (Get-FileHash $shots[0].FullName).Hash -ne (Get-FileHash $shots[1].FullName).Hash }
Check "les 5 torrents de l'inbox sont montes ($ready)"   { $ready -ge 5 }
Check "aucun echec d'ecriture SD"                        { $logText -notmatch 'ÉCHEC' }
Check "l'inbox a ete videe apres import ($inbox)"        { $inbox -eq 0 }
Check "les points de reprise sont ecrits ($resume)"      { $resume -ge 5 }
Check "les captures sont produites ($($shots.Count))"    { $shots.Count -ge 4 }
Check "aucune capture vide ($blank)"                     { $blank -eq 0 }

Remove-Item -Recurse -Force $seed -ErrorAction SilentlyContinue

Write-Host ''
if ($script:fail -eq 0) { Write-Host "tout est vert — captures dans $Out" -ForegroundColor Green; exit 0 }
Write-Host "$($script:fail) verification(s) en echec" -ForegroundColor Red
exit 1
