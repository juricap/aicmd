param([Parameter(Mandatory=$true)][int]$ProcessId, [string]$Output = "$PSScriptRoot\..\artifacts\window.png")
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public static class WindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int cmd);
    public static void Save(IntPtr h,string path) {
        Rect r; GetWindowRect(h,out r);
        using (var bmp = new Bitmap(r.Right-r.Left,r.Bottom-r.Top)) {
            using (var g = Graphics.FromImage(bmp)) {
                IntPtr dc = g.GetHdc();
                try { if (!PrintWindow(h,dc,2)) throw new Exception("PrintWindow failed"); }
                finally { g.ReleaseHdc(dc); }
            }
            bmp.Save(path,System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
'@
$process = Get-Process -Id $ProcessId
$handle = $process.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) { throw 'Process has no visible main window.' }
[WindowCapture]::Save($handle,[IO.Path]::GetFullPath($Output))
Get-Item $Output | Select-Object FullName,Length
