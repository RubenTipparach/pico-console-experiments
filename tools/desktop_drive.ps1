# Launch a game's Windows desktop build, press buttons at it, and photograph
# the window. This is how a change to anything drawn with the SDK (the HUD,
# the menus, the catch card) gets checked, because none of that goes through
# pse::RenderTarget and so none of it appears in the preview harness.
#
#   tools\desktop_drive.ps1 -Slug kingfisher -Keys "DOWN,Z" -Shots 6
#
# Keys is a comma separated sequence of:
#   Z X C V DOWN UP LEFT RIGHT   a tap (A B X Y and the dpad)
#   hold:Z:5000                  hold a button down for 5000 ms
#   wait:1500                    do nothing for 1500 ms
#   shot:name                    photograph the window now
#
# Three traps live in here, all of them found the hard way:
#
#  1. $proc.MainWindowHandle is the process's CONSOLE window, not the game.
#     Screenshots of it are a black rectangle with the SDL banner text, or
#     whatever else happened to be on the desktop. The game window is found
#     by class name, SDL_app, among the process's own top level windows.
#
#  2. SendKeys does not reach SDL at all, and keybd_event with scan = 0
#     delivers the arrow keys but not the letters. SDL resolves a key from
#     the hardware scan code in the message, so the scan code has to be real:
#     MapVirtualKey(vk, 0). Without it the dpad works, Z does nothing, and it
#     looks exactly like Z is not the A button. It is (32blit-sdl/Input.cpp).
#
#  3. A tap has to be held across several frames. The game reads edge
#     triggered `buttons.pressed`, and a press and release inside one frame
#     can land between two polls and be missed.

param(
    [Parameter(Mandatory = $true)] [string] $Slug,
    [string] $Keys = "",
    [int]    $Shots = 0,
    [int]    $IntervalMs = 5000,
    [string] $Out = "",
    [string] $Exe = ""
)

Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class BlitWin {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }

  public static IntPtr FindGameWindow(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      uint wpid; GetWindowThreadProcessId(h, out wpid);
      if (wpid != pid) return true;
      var sb = new StringBuilder(256);
      GetClassName(h, sb, sb.Capacity);
      if (sb.ToString() == "SDL_app") { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  static uint Ext(byte vk) {
    return (vk == 0x25 || vk == 0x26 || vk == 0x27 || vk == 0x28) ? 1u : 0u;
  }
  public static void KeyDown(byte vk) {
    keybd_event(vk, (byte)MapVirtualKey(vk, 0), Ext(vk), UIntPtr.Zero);
  }
  public static void KeyUp(byte vk) {
    keybd_event(vk, (byte)MapVirtualKey(vk, 0), Ext(vk) | 2u, UIntPtr.Zero);
  }
}
"@

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    $dir = Join-Path $root "build.desktop/$Slug"
    if (-not (Test-Path $dir)) {
        Write-Output "No desktop build at $dir. Run run_$Slug.bat first."
        exit 1
    }
    $found = Get-ChildItem -Recurse $dir -Filter "$Slug.exe" -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if (-not $found) { Write-Output "No $Slug.exe under $dir"; exit 1 }
    $Exe = $found.FullName
}
if (-not $Out) { $Out = Join-Path $root "build.desktop/$Slug/shots" }
New-Item -ItemType Directory -Force $Out | Out-Null

# --size 240,240 for the same reason the web shell passes it: the SDL backend
# defaults to 320x240 while the board is 240x240.
$proc = Start-Process -FilePath $Exe -ArgumentList "--size","240,240" -PassThru
Start-Sleep -Milliseconds 3000

$h = [BlitWin]::FindGameWindow($proc.Id)
if ($h -eq [IntPtr]::Zero) {
    Write-Output "No SDL_app window for $Slug"
    if (-not $proc.HasExited) { $proc.Kill() }
    exit 1
}
[void][BlitWin]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 900

$vks = @{ "Z" = 0x5A; "X" = 0x58; "C" = 0x43; "V" = 0x56;
          "LEFT" = 0x25; "UP" = 0x26; "RIGHT" = 0x27; "DOWN" = 0x28 }
$shot_no = 0

function Shoot([string] $name) {
    [void][BlitWin]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 250
    if ([BlitWin]::GetForegroundWindow() -ne $h) {
        Write-Output "skip $name (window not focused)"
        return
    }
    $r = New-Object BlitWin+RECT
    [void][BlitWin]::GetClientRect($h, [ref]$r)
    $p = New-Object BlitWin+POINT
    [void][BlitWin]::ClientToScreen($h, [ref]$p)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($p.X, $p.Y, 0, 0, (New-Object System.Drawing.Size $w, $ht))
    $bmp.Save("$Out/$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $gfx.Dispose(); $bmp.Dispose()
    Write-Output "shot $name ($w x $ht)"
}

function Tap([byte] $vk, [int] $holdMs) {
    [void][BlitWin]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 80
    [BlitWin]::KeyDown($vk)
    Start-Sleep -Milliseconds $holdMs
    [BlitWin]::KeyUp($vk)
    Start-Sleep -Milliseconds 220
}

Shoot "00_start"

foreach ($token in ($Keys -split "," | Where-Object { $_ -ne "" })) {
    $t = $token.Trim()
    if ($t -match "^wait:(\d+)$") {
        Start-Sleep -Milliseconds ([int]$matches[1])
    } elseif ($t -match "^shot:(.+)$") {
        Shoot $matches[1]
    } elseif ($t -match "^hold:(\w+):(\d+)$") {
        $vk = $vks[$matches[1].ToUpper()]
        if (-not $vk) { Write-Output "unknown key $($matches[1])"; continue }
        [void][BlitWin]::SetForegroundWindow($h)
        [BlitWin]::KeyDown($vk)
        Start-Sleep -Milliseconds ([int]$matches[2])
        [BlitWin]::KeyUp($vk)
        Start-Sleep -Milliseconds 300
    } else {
        $vk = $vks[$t.ToUpper()]
        if (-not $vk) { Write-Output "unknown key $t"; continue }
        Tap $vk 150
        $shot_no++
        Shoot ("{0:d2}_after_{1}" -f $shot_no, $t.ToLower())
    }
}

for ($i = 1; $i -le $Shots; $i++) {
    Start-Sleep -Milliseconds $IntervalMs
    Shoot ("9{0:d2}_running" -f $i)
}

if (-not $proc.HasExited) { $proc.Kill() }
Write-Output "shots in $Out"
