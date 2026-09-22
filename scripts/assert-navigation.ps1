param([Parameter(Mandatory=$true)][IntPtr]$Window)
$ErrorActionPreference = 'Stop'
if (-not ('NavigationProbe' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NavigationProbe {
 [StructLayout(LayoutKind.Sequential)] struct Rect { public int a,b,c,d; }
 [StructLayout(LayoutKind.Sequential)] struct Gui { public int size,flags; public IntPtr active,focus,capture,menu,move,caret; public Rect rect; }
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread,ref Gui info);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder name,int count);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint message,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint message,IntPtr w,IntPtr l);
 public static IntPtr ListFocus(IntPtr window) {
  uint pid; uint thread=GetWindowThreadProcessId(window,out pid);
  var info=new Gui(); info.size=Marshal.SizeOf(info);
  if (!GetGUIThreadInfo(thread,ref info)) return IntPtr.Zero;
  var name=new StringBuilder(256); GetClassName(info.focus,name,256);
  return name.ToString()=="SysListView32" ? info.focus : IntPtr.Zero;
 }
 public static int Row(IntPtr list) { return SendMessage(list,0x100C,new IntPtr(-1),new IntPtr(1)).ToInt32(); }
}
'@
}
$deadline = [DateTime]::UtcNow.AddSeconds(5)
do { Start-Sleep -Milliseconds 50; $list = [NavigationProbe]::ListFocus($Window) } while ($list -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
if ($list -eq [IntPtr]::Zero) { throw 'Active commander has no keyboard target in its file list after returning from viewer' }
# Let activation-triggered remote refresh finish before checking movement.
Start-Sleep -Milliseconds 750
foreach ($move in @(@(0x24,0),@(0x28,1),@(0x26,0),@(0x28,1))) {
 [NavigationProbe]::PostMessage($Window,0x100,[IntPtr]$move[0],[IntPtr]::Zero) | Out-Null
 $deadline = [DateTime]::UtcNow.AddSeconds(3)
 do { Start-Sleep -Milliseconds 30; $row = [NavigationProbe]::Row($list) } while ($row -ne $move[1] -and [DateTime]::UtcNow -lt $deadline)
 if ($row -ne $move[1]) { throw "Navigation key $($move[0]) did not move the row: expected $($move[1]), got $row" }
}
'PASS keyboard navigation after viewer exit: Home, Down, Up, Down move the cursor'
