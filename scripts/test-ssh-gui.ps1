param([string]$SshHost = "ai")
$ErrorActionPreference = 'Stop'
if ($SshHost -notmatch '^[a-zA-Z0-9][a-zA-Z0-9_.-]*$') { throw 'Use an SSH config alias' }
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SshViewerTest {
 public static IntPtr CopyDialog() { return FindWindow("#32770","Copy - F5"); }
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
if ([SshViewerTest]::Viewer() -ne [IntPtr]::Zero) { throw 'Close existing jptxt before this isolated test.' }
$fixture = Join-Path $workspace ('artifacts\viewer-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
[IO.File]::WriteAllText((Join-Path $fixture 'view space.txt'),'viewer fixture')
$remotePath = '/data/devel/aicmd-ssh-check-gui-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss')
ssh $SshHost "mkdir $remotePath"
if ($LASTEXITCODE) { throw 'Remote fixture setup failed' }
$remote = "ssh://${SshHost}${remotePath}"
& "$workspace\build\aicmd-cli.exe" copy (Join-Path $fixture 'view space.txt') "$remote/view space.txt"
if ($LASTEXITCODE) { throw 'Fixture upload failed' }
$destination = Join-Path $fixture 'destination'
[IO.Directory]::CreateDirectory($destination) | Out-Null
$session = $fixture + '.session.json'
$process = Start-Process "$workspace\build\aicmd.exe" -ArgumentList @('--session',('"'+$session+'"'),('"'+$remote+'"'),('"'+$destination+'"')) -WindowStyle Hidden -PassThru
$viewer = [IntPtr]::Zero
try {
 Start-Sleep -Seconds 3
 $process.Refresh(); $window = [SshViewerTest]::Commander()
 [SshViewerTest]::ShowWindow($window,9) | Out-Null
 [SshViewerTest]::SetForegroundWindow($window) | Out-Null
 Start-Sleep -Milliseconds 300
 [SshViewerTest]::PostMessage($window,0x100,[IntPtr]0x28,[IntPtr]::Zero) | Out-Null
 [SshViewerTest]::PostMessage($window,0x100,[IntPtr]0x6a,[IntPtr]::Zero) | Out-Null
 Start-Sleep -Milliseconds 250
 & "$PSScriptRoot\capture-window.ps1" -ProcessId $process.Id -Output "$workspace\artifacts\ssh-gui.png"
 [SshViewerTest]::PostMessage($window,0x100,[IntPtr]0x72,[IntPtr]::Zero) | Out-Null
 $deadline = [DateTime]::UtcNow.AddSeconds(10)
 do { Start-Sleep -Milliseconds 100; $viewer = [SshViewerTest]::Viewer() } while ($viewer -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
 if ($viewer -eq [IntPtr]::Zero -or [SshViewerTest]::Title($viewer) -notmatch '\[VIEW\]') { throw 'F3 did not open a read-only jptxt window' }
 [SshViewerTest]::PostMessage($viewer,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
 $deadline = [DateTime]::UtcNow.AddSeconds(5)
 while ([SshViewerTest]::Viewer() -ne [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
 $viewer = [IntPtr]::Zero
 [SshViewerTest]::SetForegroundWindow($window) | Out-Null
 & "$PSScriptRoot\assert-navigation.ps1" -Window $window
 [SshViewerTest]::PostMessage($window,0x100,[IntPtr]0x74,[IntPtr]::Zero) | Out-Null
 $deadline = [DateTime]::UtcNow.AddSeconds(5)
 do { Start-Sleep -Milliseconds 100; $dialog = [SshViewerTest]::CopyDialog() } while ($dialog -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
 if ($dialog -eq [IntPtr]::Zero) { throw 'F5 confirmation did not open' }
 [SshViewerTest]::PostMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero) | Out-Null
 $download = Join-Path $destination 'view space.txt'
 $deadline = [DateTime]::UtcNow.AddSeconds(15)
 while (-not (Test-Path -LiteralPath $download) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
 if (-not (Test-Path -LiteralPath $download) -or [IO.File]::ReadAllText($download) -ne 'viewer fixture') { throw 'F5 remote download failed' }
 'PASS SSH GUI: remote browsing, F3 jptxt read-only and F5 download'
} finally {
 if ($viewer -ne [IntPtr]::Zero) { [SshViewerTest]::PostMessage($viewer,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null }
 if (-not $process.HasExited) { $process.CloseMainWindow() | Out-Null; if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id } }
}
