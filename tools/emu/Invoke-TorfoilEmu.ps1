param(
    [string]$Nro = "$PSScriptRoot\..\..\torfoil.nro",
    [string]$Script = 'shot,quit',
    [string]$Sd = '',
    [string]$Out = "$PSScriptRoot\..\..\emu-out",
    [string]$EmuHome = "$env:LOCALAPPDATA\torfoil-emu",
    [switch]$Net,
    [int]$WarmupSeconds = 90,
    [int]$TimeoutSeconds = 600,
    [int]$KeyHoldMs = 700,
    [string]$GraphicsBackend = 'Vulkan'
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public static class Emu {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static IntPtr FindByPid(uint target) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != target) return true;
            int len = GetWindowTextLength(h);
            if (len < 1) return true;
            var sb = new System.Text.StringBuilder(len + 1);
            GetWindowText(h, sb, sb.Capacity);
            string title = sb.ToString();
            if (title.StartsWith("Ryujinx") && title.Contains(" - ")) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {
        public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT {
        public uint type; public KEYBDINPUT ki; public int pad1; public int pad2;
    }
    [DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);

    private static void Send(ushort scan, bool extended, bool up) {
        var inp = new INPUT[1];
        inp[0].type = 1;
        inp[0].ki.wVk = 0;
        inp[0].ki.wScan = scan;
        inp[0].ki.dwFlags = 0x0008u | (extended ? 0x0001u : 0u) | (up ? 0x0002u : 0u);
        SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void Key(ushort scan, bool extended, int holdMs) {
        Send(scan, extended, false);
        System.Threading.Thread.Sleep(holdMs);
        Send(scan, extended, true);
    }

    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);

    public static void Shot(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        using (var bmp = new Bitmap(w, ht))
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr hdc = g.GetHdc();
            bool ok = PrintWindow(h, hdc, 2);
            g.ReleaseHdc(hdc);
            if (!ok) { g.CopyFromScreen(r.Left, r.Top, 0, 0, new Size(w, ht)); }
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
'@ -ReferencedAssemblies System.Drawing, System.Windows.Forms

$exe = Join-Path $EmuHome 'app\Ryujinx.exe'
$data = Join-Path $EmuHome 'data'
if (-not (Test-Path $exe)) { throw "emulateur absent : $exe (lance Install-TorfoilEmu.ps1)" }

$Nro = (Resolve-Path $Nro).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Get-ChildItem "$Out\shot-*.png" -ErrorAction SilentlyContinue | Remove-Item -Force

if ($Sd) {
    Remove-Item -Recurse -Force "$data\sdcard" -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path "$data\sdcard" | Out-Null
    Copy-Item "$Sd\*" "$data\sdcard\" -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path "$data\sdcard" | Out-Null

$profileDir = Join-Path $data 'profiles\keyboard'
New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
Copy-Item "$PSScriptRoot\profile-torfoil.json" "$profileDir\torfoil.json" -Force

$args = @('--no-gui', '--root-data-dir', $data, '--graphics-backend', $GraphicsBackend,
          '--enable-keyboard', '--ignore-missing-services', '--hide-cursor', 'Always',
          '--input-id-1', '0', '--input-profile-1', 'torfoil')
if ($Net) { $args += '--enable-internet-connection' }
$args += $Nro

$proc = Start-Process -FilePath $exe -ArgumentList $args -PassThru `
    -RedirectStandardOutput "$Out\emulator.log" -RedirectStandardError "$Out\emulator.err"

$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt $WarmupSeconds; $i++) {
    if ($proc.HasExited) { throw "l'emulateur s'est arrete (code $($proc.ExitCode))" }
    $hwnd = [Emu]::FindByPid([uint32]$proc.Id)
    if ($hwnd -ne [IntPtr]::Zero) { break }
    Start-Sleep -Seconds 1
}
if ($hwnd -eq [IntPtr]::Zero) { $proc.Kill(); throw "aucune fenetre apres $WarmupSeconds s" }
[Emu]::ShowWindow($hwnd, 5) | Out-Null
[Emu]::SetForegroundWindow($hwnd) | Out-Null

$settle = "$Out\.settle.png"
$prev = ''
$ready = $false
for ($i = 0; $i -lt $WarmupSeconds; $i++) {
    Start-Sleep -Seconds 1
    try { [Emu]::Shot($hwnd, $settle) } catch { continue }
    $size = (Get-Item $settle).Length
    if ($size -gt 20000 -and $prev -eq 'plein') { $ready = $true; break }
    $prev = if ($size -gt 20000) { 'plein' } else { 'vide' }
}
Remove-Item $settle -ErrorAction SilentlyContinue
if (-not $ready) { $proc.Kill(); throw "l'image ne s'est jamais stabilisee" }
Start-Sleep -Seconds 2

$vk = @{ 'A' = @(0x39, $false); 'B' = @(0x0E, $false); 'X' = @(0x52, $true); 'Y' = @(0x53, $true)
         'L' = @(0x47, $true); 'R' = @(0x4F, $true); 'ZL' = @(0x49, $true); 'ZR' = @(0x51, $true)
         '+' = @(0x1C, $false); '-' = @(0x0F, $false)
         'Haut' = @(0x48, $true); 'Bas' = @(0x50, $true); 'Gauche' = @(0x4B, $true); 'Droite' = @(0x4D, $true)
         'Up' = @(0x48, $true); 'Down' = @(0x50, $true); 'Left' = @(0x4B, $true); 'Right' = @(0x4D, $true) }

$probe = "$Out\.probe.png"
function Frame-Hash {
    try { [Emu]::Shot($hwnd, $probe); return (Get-FileHash $probe -Algorithm MD5).Hash } catch { return '' }
}

$shot = 0
$insistFailures = 0
foreach ($raw in $Script.Split(',')) {
    $step = $raw.Trim()
    if (-not $step) { continue }
    if ($proc.HasExited) { break }
    [Emu]::SetForegroundWindow($hwnd) | Out-Null

    $insist = $step.EndsWith('!')
    if ($insist) { $step = $step.TrimEnd('!') }

    if ($vk.ContainsKey($step) -and $insist) {
        $done = $false
        for ($try = 1; $try -le 6 -and -not $done; $try++) {
            $before = Frame-Hash
            for ($s = 0; $s -lt 12; $s++) {
                Start-Sleep -Milliseconds 400
                $now = Frame-Hash
                if ($now -eq $before) { break }
                $before = $now
            }
            [Emu]::Key([uint16]$vk[$step][0], [bool]$vk[$step][1], $KeyHoldMs)
            Start-Sleep -Milliseconds 900
            if ((Frame-Hash) -ne $before) { $done = $true }
        }
        if (-not $done) { Write-Warning "$step sans effet visible apres 6 essais"; $insistFailures++ }
    }
    elseif ($vk.ContainsKey($step)) {
        [Emu]::Key([uint16]$vk[$step][0], [bool]$vk[$step][1], $KeyHoldMs)
        Start-Sleep -Milliseconds 600
    }
    elseif ($step -match '^wait\s*(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]) }
    elseif ($step -eq 'shot') {
        $shot++
        $path = Join-Path $Out ('shot-{0:d2}.png' -f $shot)
        [Emu]::Shot($hwnd, $path)
        Write-Output "capture $(Split-Path $path -Leaf)"
    }
    elseif ($step -eq 'quit') { [Emu]::Key([uint16]0x1C, $false, 250); Start-Sleep -Seconds 3 }
    else { Write-Warning "action inconnue : $step" }
}

Remove-Item $probe -ErrorAction SilentlyContinue
if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(15000) | Out-Null }

Remove-Item -Recurse -Force "$Out\sdcard" -ErrorAction SilentlyContinue
Copy-Item "$data\sdcard" "$Out\sdcard" -Recurse -Force

if ($insistFailures -gt 0) { throw "$insistFailures touche(s) sans effet visible" }
Write-Output "captures : $Out"
Write-Output "carte SD : $Out\sdcard"
if (Select-String -Path "$Out\emulator.log" -Pattern 'Unhandled exception|Fatal error' -Quiet) {
    throw "l'emulateur a signale une erreur, voir $Out\emulator.log"
}
