$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$fixture = Join-Path $workspace ('artifacts\media-commander-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
& ffmpeg -hide_banner -loglevel error -f lavfi -i 'color=c=red:size=320x240' -frames:v 1 -y "$fixture\a red.png"
if ($LASTEXITCODE) {throw 'Fixture generation failed'}
Copy-Item -LiteralPath "$fixture\a red.png" -Destination "$fixture\b red.png"
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class MediaCommanderTest {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int n);
 [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 public static IntPtr Commander() {return FindWindow("aicmd-window",null);}
 public static IntPtr Viewer() {return FindWindow("aiview-window",null);}
 public static void Key(IntPtr h,int key) {PostMessage(h,0x100,(IntPtr)key,IntPtr.Zero);}
 public static int Row(IntPtr list) {return SendMessage(list,0x100c,new IntPtr(-1),new IntPtr(1)).ToInt32();}
 public static bool Mark(IntPtr list,int row) {return SendMessage(list,0x102c,new IntPtr(row),new IntPtr(2)).ToInt32()!=0;}
}
'@
if ([MediaCommanderTest]::Commander() -ne [IntPtr]::Zero -or [MediaCommanderTest]::Viewer() -ne [IntPtr]::Zero) {throw 'Close aicmd/aiview before this isolated test.'}
$process = Start-Process "$workspace\build\aicmd.exe" -ArgumentList @('--session',('"'+$fixture+'.session.json"'),('"'+$fixture+'"'),('"'+$fixture+'"')) -WindowStyle Hidden -PassThru
$viewer = [IntPtr]::Zero
try {
 Start-Sleep -Seconds 1
 $window = [MediaCommanderTest]::Commander(); [MediaCommanderTest]::ShowWindow($window,9) | Out-Null; [MediaCommanderTest]::SetForegroundWindow($window) | Out-Null
 Start-Sleep -Milliseconds 200
 [MediaCommanderTest]::Key($window,0x24); [MediaCommanderTest]::Key($window,0x28); [MediaCommanderTest]::Key($window,0x2d); [MediaCommanderTest]::Key($window,0x26)
 Start-Sleep -Milliseconds 150
 [MediaCommanderTest]::Key($window,0x20)
 $deadline = [DateTime]::UtcNow.AddSeconds(10)
 do { Start-Sleep -Milliseconds 100; $viewer = [MediaCommanderTest]::Viewer() } while ($viewer -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
 if ($viewer -eq [IntPtr]::Zero) {throw 'Space did not launch aiview'}
 Start-Sleep -Milliseconds 600
 [MediaCommanderTest]::Key($viewer,0x22); Start-Sleep -Milliseconds 400
 [MediaCommanderTest]::Key($viewer,0x1b)
 $deadline = [DateTime]::UtcNow.AddSeconds(5)
 while ([MediaCommanderTest]::Viewer() -ne [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {Start-Sleep -Milliseconds 50}
 $viewer = [IntPtr]::Zero; [MediaCommanderTest]::SetForegroundWindow($window) | Out-Null
 Start-Sleep -Milliseconds 800
 $list = [MediaCommanderTest]::GetDlgItem($window,104)
 if ([MediaCommanderTest]::Row($list) -ne 2) {throw 'Viewer did not return the browsed item to commander'}
 if (-not [MediaCommanderTest]::Mark($list,1) -or [MediaCommanderTest]::Mark($list,2)) {throw 'Viewer launch/return changed selection marks'}
 & "$PSScriptRoot\assert-navigation.ps1" -Window $window
 'PASS Space launch with spaced filename, folder PageDown, Escape, current-item return, independent marks and keyboard recovery'
} finally {
 if ($viewer -ne [IntPtr]::Zero) {[MediaCommanderTest]::PostMessage($viewer,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null}
 if (-not $process.HasExited) {$process.CloseMainWindow() | Out-Null; if (-not $process.WaitForExit(5000)) {Stop-Process -Id $process.Id}}
}
