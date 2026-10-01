$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$fixture = Join-Path $workspace ('artifacts\copy-gui-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
[IO.Directory]::CreateDirectory("$fixture\source\bulk") | Out-Null
[IO.Directory]::CreateDirectory("$fixture\dest") | Out-Null
$bytes = New-Object byte[] 8192
for ($i=0;$i -lt 2000;$i++) {[IO.File]::WriteAllBytes(("$fixture\source\bulk\file-{0:D4}.bin" -f $i),$bytes)}
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class CopyGui {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,IntPtr p);
 [DllImport("user32.dll")] static extern bool SetKeyboardState(byte[] state);
 public static IntPtr Window() {return FindWindow("aicmd-window",null);}
 public static IntPtr Dialog() {return FindWindow("#32770","Copy - F5");}
 public static string Status(IntPtr h) {var s=new StringBuilder(4096); GetWindowText(GetDlgItem(h,50),s,s.Capacity); return s.ToString();}
 public static void Key(IntPtr h,int k) {PostMessage(h,0x100,(IntPtr)k,IntPtr.Zero);}
 public static void CancelAll(IntPtr h) {uint own=GetCurrentThreadId(), target=GetWindowThreadProcessId(h,IntPtr.Zero); if (!AttachThreadInput(own,target,true)) throw new Exception("Cannot attach input queue"); try {var state=new byte[256]; state[0x10]=0x80; state[0xa0]=0x80; SetKeyboardState(state); Key(h,0x1b); System.Threading.Thread.Sleep(150); SetKeyboardState(new byte[256]);} finally {AttachThreadInput(own,target,false);}}
}
'@
if ([CopyGui]::Window() -ne [IntPtr]::Zero) {throw 'Close aicmd before this isolated GUI test'}
$p = Start-Process "$workspace\build\aicmd.exe" -ArgumentList @('--session',('"'+$fixture+'.json"'),('"'+$fixture+'\source"'),('"'+$fixture+'\dest"')) -WindowStyle Hidden -PassThru
function Wait-For([scriptblock]$condition) { $deadline=[DateTime]::UtcNow.AddSeconds(15); do {if (& $condition) {return}; Start-Sleep -Milliseconds 20} while ([DateTime]::UtcNow -lt $deadline); throw "GUI wait failed: $([CopyGui]::Status($window))" }
function Enqueue {
 [CopyGui]::Key($window,0x74)
 Wait-For { [CopyGui]::Dialog() -ne [IntPtr]::Zero }
 [CopyGui]::PostMessage([CopyGui]::Dialog(),0x111,[IntPtr]1,[IntPtr]::Zero) | Out-Null
 Wait-For { [CopyGui]::Dialog() -eq [IntPtr]::Zero }
}
try {
 Start-Sleep -Seconds 1; $window=[CopyGui]::Window()
 [CopyGui]::ShowWindow($window,9) | Out-Null; [CopyGui]::SetForegroundWindow($window) | Out-Null
 Start-Sleep -Milliseconds 150
 [CopyGui]::Key($window,0x6a) # Num* marks the complete large selection.
 Enqueue
 Wait-For { [CopyGui]::Status($window) -match 'bytes.*1 selected entries' }
 Enqueue
 Wait-For { [CopyGui]::Status($window) -match '1 pending' }
 & "$PSScriptRoot\capture-window.ps1" -ProcessId $p.Id -Output "$fixture\queued.png"
 [CopyGui]::Key($window,0x28)
 [CopyGui]::SetForegroundWindow($window) | Out-Null
 Start-Sleep -Milliseconds 150
 [CopyGui]::CancelAll($window)
 Wait-For { [CopyGui]::Status($window) -match 'Transfers finished:.*2 cancelled' }
 if (Get-ChildItem "$fixture\dest" -Recurse -Filter '.aicmd-part-*') {throw 'Staging files remain after acknowledged cancellation'}
 & "$PSScriptRoot\assert-navigation.ps1" -Window $window
 'PASS native 2000-file progress, second queued batch, Shift+Escape cancellation, cleanup and navigation'
} finally {if (-not $p.HasExited) {$p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(5000)) {Stop-Process -Id $p.Id}}}
