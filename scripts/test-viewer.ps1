$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ViewerTest {
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,StringBuilder b,int n);
 public static IntPtr Commander() { return FindWindow("aicmd-window",null); }
 public static IntPtr Viewer() { return FindWindow("jptxt",null); }
 public static string Title(IntPtr h) { var b=new StringBuilder(2048); GetWindowText(h,b,b.Capacity); return b.ToString(); }
}
'@
if ([ViewerTest]::Viewer() -ne [IntPtr]::Zero) { throw 'Close existing jptxt before this isolated test.' }
$fixture = Join-Path $workspace ('artifacts\viewer-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
[IO.File]::WriteAllText((Join-Path $fixture 'view space.txt'),'viewer fixture')
$session = $fixture + '.session.json'
$process = Start-Process "$workspace\build\aicmd.exe" -ArgumentList @('--session',('"'+$session+'"'),('"'+$fixture+'"'),('"'+$fixture+'"')) -WindowStyle Hidden -PassThru
$viewer = [IntPtr]::Zero
try {
 Start-Sleep -Seconds 2
 $process.Refresh(); $window = [ViewerTest]::Commander()
 [ViewerTest]::ShowWindow($window,9) | Out-Null
 [ViewerTest]::SetForegroundWindow($window) | Out-Null
 Start-Sleep -Milliseconds 300
 [ViewerTest]::PostMessage($window,0x100,[IntPtr]0x28,[IntPtr]::Zero) | Out-Null
 [ViewerTest]::PostMessage($window,0x100,[IntPtr]0x6a,[IntPtr]::Zero) | Out-Null
 Start-Sleep -Milliseconds 250
 & "$PSScriptRoot\capture-window.ps1" -ProcessId $process.Id -Output "$workspace\artifacts\marked.png"
 [ViewerTest]::PostMessage($window,0x100,[IntPtr]0x72,[IntPtr]::Zero) | Out-Null
 $deadline = [DateTime]::UtcNow.AddSeconds(10)
 do { Start-Sleep -Milliseconds 100; $viewer = [ViewerTest]::Viewer() } while ($viewer -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
 if ($viewer -eq [IntPtr]::Zero -or [ViewerTest]::Title($viewer) -notmatch '\[VIEW\]') { throw 'F3 did not open a read-only jptxt window' }
 [ViewerTest]::PostMessage($viewer,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
 $deadline = [DateTime]::UtcNow.AddSeconds(5)
 while ([ViewerTest]::Viewer() -ne [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
 $viewer = [IntPtr]::Zero
 [ViewerTest]::SetForegroundWindow($window) | Out-Null
 & "$PSScriptRoot\assert-navigation.ps1" -Window $window
 'PASS aicmd F3 launches jptxt read-only with a spaced filename'
} finally {
 if ($viewer -ne [IntPtr]::Zero) { [ViewerTest]::PostMessage($viewer,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
 if (-not $process.HasExited) { $process.CloseMainWindow() | Out-Null; if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id } }
}
