# Captures what the screen shows of a game window, for tools/motion.py: a band of the window's client area, every
# desktop update (Desktop Duplication through ffmpeg's ddagrab, no duplicated frames), as 8-bit grey frames plus
# each frame's time. It only reads the screen: nothing in the game or in Windows is changed.
#
#   motion_capture.ps1 -Process wonder2 -Seconds 20 -Out <dir> [-Delay 0] [-Band 0.55,0.30] [-Width 640]
#
# Band: top and height of the captured band as fractions of the client area (default: a strip below the middle,
# where the ground and the player are). Width: the band is scaled to this width (height keeps the aspect).
param([string]$Process = "wonder2", [double]$Seconds = 20, [string]$Out, [double]$Delay = 0,
      [double[]]$Band = @(0.45, 0.35), [int]$Width = 640, [string]$Ffmpeg = "C:\ffmpeg\bin\ffmpeg.exe")
$ErrorActionPreference = "Stop"
Add-Type -Namespace W -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetClientRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(System.IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr l);
public static System.IntPtr Largest(uint pid) {
    System.IntPtr best = System.IntPtr.Zero; long area = 0;
    EnumWindows((h, l) => {
        uint p; GetWindowThreadProcessId(h, out p);
        RECT r;
        if (p == pid && IsWindowVisible(h) && GetClientRect(h, out r)) {
            long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (a > area) { area = a; best = h; }
        }
        return true;
    }, System.IntPtr.Zero);
    return best;
}
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
'@
[W.U]::SetProcessDPIAware() | Out-Null
if ($Delay -gt 0) { Start-Sleep -Milliseconds ([int]($Delay * 1000)) }
$hwnd = [IntPtr]::Zero
foreach ($p in @(Get-Process -Name $Process -ErrorAction Stop)) {
    $hwnd = [W.U]::Largest([uint32]$p.Id)
    if ($hwnd -ne [IntPtr]::Zero) { break }
}
if ($hwnd -eq [IntPtr]::Zero) { throw "no visible window for process $Process" }
$r = New-Object W.U+RECT
[W.U]::GetClientRect($hwnd, [ref]$r) | Out-Null
$pt = New-Object W.U+POINT
[W.U]::ClientToScreen($hwnd, [ref]$pt) | Out-Null
$cw = $r.Right - $r.Left; $ch = $r.Bottom - $r.Top
$y = $pt.Y + [int]($ch * $Band[0]); $h = [int]($ch * $Band[1]) -band -2; $w = $cw -band -2
$oh = [int]($Width * $h / $w) -band -2
New-Item -ItemType Directory -Force $Out | Out-Null
"client {0}x{1} at {2},{3}; band {4}x{5} at {2},{6} -> {7}x{8}" -f $cw, $ch, $pt.X, $pt.Y, $w, $h, $y, $Width, $oh |
    Tee-Object -FilePath "$Out\capture.txt"
$vf = "hwdownload,format=bgra,format=gray,scale=${Width}:${oh}:flags=area,showinfo"
$args_ = @("-hide_banner", "-loglevel", "info", "-f", "lavfi", "-i",
    "ddagrab=output_idx=0:draw_mouse=0:framerate=240:dup_frames=0:offset_x=$($pt.X):offset_y=${y}:video_size=${w}x${h}",
    "-t", "$Seconds", "-vf", $vf, "-fps_mode", "passthrough", "-f", "rawvideo", "-y", "`"$(Join-Path $Out 'frames.gray')`"")
Start-Process -FilePath $Ffmpeg -ArgumentList $args_ -NoNewWindow -Wait -RedirectStandardError (Join-Path $Out 'ffmpeg.log') `
    -RedirectStandardOutput (Join-Path $Out 'ffmpeg.out')
$line = "frames: {0}" -f (Select-String -Path (Join-Path $Out 'ffmpeg.log') -Pattern "pts_time:").Count; $line; Add-Content -Encoding ascii -Path (Join-Path $Out 'capture.txt') -Value $line
Add-Content -Encoding ascii -Path (Join-Path $Out 'capture.txt') -Value "size ${Width}x${oh}"
