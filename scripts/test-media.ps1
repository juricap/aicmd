param([switch]$Ssh)
$ErrorActionPreference = 'Stop'
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$fixture = Join-Path $workspace ('artifacts\media-check-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
function FF { & ffmpeg -hide_banner -loglevel error @args; if ($LASTEXITCODE) { throw 'FFmpeg fixture failed' } }
FF -f lavfi -i 'color=c=red:size=320x240' '-frames:v' 1 -y "$fixture\01-red.jpg"
FF -f lavfi -i 'color=c=blue:size=240x320' '-frames:v' 1 -y "$fixture\02-blue.png"
FF -f lavfi -i 'testsrc2=size=640x360:rate=24' -f lavfi -i 'sine=frequency=440:sample_rate=48000' -t 12 '-c:v' libx264 -g 48 -pix_fmt yuv420p '-c:a' aac -y "$fixture\03-video.mp4"
FF -f lavfi -i 'sine=frequency=220:sample_rate=48000' -t 12 -y "$fixture\04-audio.wav"
[IO.File]::WriteAllText("$fixture\05-broken.mp4",'not a video')
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.IO;
public static class MediaTest {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] static extern void keybd_event(byte k,byte s,uint f,UIntPtr x);
 [DllImport("user32.dll")] static extern bool OpenClipboard(IntPtr h);
 [DllImport("user32.dll")] static extern bool CloseClipboard();
 [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint f);
 [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr h);
 [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr h);
 public static IntPtr Window() { return FindWindow("aiview-window",null); }
 public static void Key(IntPtr h,int key) { PostMessage(h,0x100,(IntPtr)key,IntPtr.Zero); }
 public static void CopyKey(IntPtr h) { SetForegroundWindow(h); keybd_event(0x11,0,0,UIntPtr.Zero); keybd_event(0x43,0,0,UIntPtr.Zero); keybd_event(0x43,0,2,UIntPtr.Zero); keybd_event(0x11,0,2,UIntPtr.Zero); }
 public static int[] Clipboard(string path) {
  if (!OpenClipboard(IntPtr.Zero)) throw new Exception("Cannot open clipboard");
  try { var h=GetClipboardData(8); if (h==IntPtr.Zero) throw new Exception("No CF_DIB image"); var p=GlobalLock(h);
   try { int header=Marshal.ReadInt32(p),w=Marshal.ReadInt32(p,4),height=Marshal.ReadInt32(p,8); int size=header+w*Math.Abs(height)*4;
    byte[] data=new byte[size]; Marshal.Copy(p,data,0,size);
    using (var output=new BinaryWriter(File.Create(path))) {output.Write((ushort)0x4d42); output.Write(size+14); output.Write(0); output.Write(header+14); output.Write(data);}
    int other=header+(Math.Abs(height)/2*w+w*3/4)*4;
    return new int[]{w,Math.Abs(height),data[header],data[header+1],data[header+2],data[other],data[other+1],data[other+2]};
   } finally {GlobalUnlock(h);}
  } finally {CloseClipboard();}
 }
}
'@
if ([MediaTest]::Window() -ne [IntPtr]::Zero) { throw 'Close the existing aiview window before running this isolated test.' }
$process = Start-Process "$workspace\build\aiview.exe" -ArgumentList @('--',('"'+$fixture+'\01-red.jpg"')) -WindowStyle Hidden -PassThru
$script:checks = 0
function Check($ok,$message) { if (-not $ok) { throw $message }; $script:checks++ }
function Tool($name,$arguments = @{}) {
 $request = @{name=$name; arguments=$arguments} | ConvertTo-Json -Compress -Depth 8
 $result = $request | & "$workspace\build\aiview-tool.exe" --pipe "aiview-$($process.Id)" | ConvertFrom-Json
 if (-not $result.ok) { throw "$name : $($result.error)" }; return $result.result
}
function Wait-State([scriptblock]$condition) {
 $deadline = [DateTime]::UtcNow.AddSeconds(15)
 do { Start-Sleep -Milliseconds 80; $state = Tool media_get_state; if (& $condition $state) { return $state } } while ([DateTime]::UtcNow -lt $deadline)
 throw "Timed out waiting for media state: $($state | ConvertTo-Json -Compress -Depth 8)"
}
try {
 Start-Sleep -Seconds 1
 $s = Wait-State {param($s) $s.ready -and $s.index -eq 0}
 Check ($s.count -eq 5 -and $s.width -eq 320 -and $s.height -eq 240) 'JPEG or folder enumeration failed'
 $window = [MediaTest]::Window(); [MediaTest]::ShowWindow($window,9) | Out-Null; [MediaTest]::SetForegroundWindow($window) | Out-Null
 Start-Sleep -Milliseconds 200
 [MediaTest]::CopyKey($window)
 $s = Wait-State {param($s) $s.last_operation.status -eq 'completed'}
 $pixels = [MediaTest]::Clipboard("$fixture\clipboard-red.bmp")
 Check ($pixels[0] -eq 320 -and $pixels[1] -eq 240 -and $pixels[4] -gt 240 -and $pixels[2] -lt 10) 'Ctrl+C did not copy decoded red image pixels'
 [MediaTest]::Key($window,0x22)
 $s = Wait-State {param($s) $s.ready -and $s.index -eq 1}
 Check ($s.width -eq 240 -and $s.height -eq 320) 'PageDown PNG navigation failed'
 [MediaTest]::Key($window,0x22)
 $s = Wait-State {param($s) $s.ready -and $s.index -eq 2 -and $s.position -gt 0.2}
 Check (-not $s.paused -and $s.duration -eq 12) 'MP4 did not autoplay'
 Tool media_pause | Out-Null; $null = Wait-State {param($s) $s.paused}
 Tool media_seek_absolute @{seconds=2} | Out-Null; $null = Wait-State {param($s) [Math]::Abs($s.position-2) -lt 0.03 -and -not $s.seeking}
 [MediaTest]::Key($window,0x27)
 $s = Wait-State {param($s) [Math]::Abs($s.position-7) -lt 0.03 -and -not $s.seeking}
 Check $s.paused 'Right seek did not retain pause'
 [MediaTest]::Key($window,0x25)
 $s = Wait-State {param($s) [Math]::Abs($s.position-2) -lt 0.03 -and -not $s.seeking}
 Check $s.paused 'Left seek did not retain pause'
 [MediaTest]::Key($window,0xbe)
 $s = Wait-State {param($s) [Math]::Abs($s.position-(2+1.0/24)) -lt 0.005}
 Check $s.paused 'Forward frame did not remain paused'
 [MediaTest]::Key($window,0xbc)
 $s = Wait-State {param($s) [Math]::Abs($s.position-2) -lt 0.005}
 Check $s.paused 'Backward frame did not remain paused'
 $op = Tool media_copy_frame
 $null = Wait-State {param($s) $s.last_operation.id -eq $op.operation -and $s.last_operation.status -eq 'completed'}
 $pixels = [MediaTest]::Clipboard("$fixture\clipboard-video.bmp")
 Check ($pixels[0] -eq 640 -and $pixels[1] -eq 360) 'Video frame copy dimensions are wrong'
 $op = Tool media_capture_frame
 $null = Wait-State {param($s) $s.last_operation.id -eq $op.operation -and $s.last_operation.status -eq 'completed'}
 $frame = Tool media_get_frame @{operation=$op.operation}
 [IO.File]::WriteAllBytes("$fixture\agent-frame.png",[Convert]::FromBase64String($frame.data))
 Check ($frame.width -eq 640 -and $frame.height -eq 360 -and $frame.mimeType -eq 'image/png') 'Agent frame capture failed'
 $frameRequest = @{jsonrpc='2.0'; id=3; method='tools/call'; params=@{name='media_get_frame'; arguments=@{operation=$op.operation}}} | ConvertTo-Json -Depth 8 -Compress
 $frameReply = $frameRequest | & "$workspace\build\aiview-tool.exe" --pipe "aiview-$($process.Id)" --mcp | ConvertFrom-Json
 Check ($frameReply.result.content[0].type -eq 'image') 'MCP did not return image content'
 & "$PSScriptRoot\capture-window.ps1" -ProcessId $process.Id -Output "$fixture\video-window.png"
 $old = $s.generation
 [MediaTest]::Key($window,0x23)
 $s = Wait-State {param($s) $s.index -eq 4 -and $s.error.Length -gt 0}
 Check (-not $s.ready) 'Corrupt video should report an error'
 $bad = '{"name":"media_pause","arguments":{"generation":'+$old+'}}' | & "$workspace\build\aiview-tool.exe" --pipe "aiview-$($process.Id)" | ConvertFrom-Json
 Check (-not $bad.ok -and $bad.error -match 'Stale') 'Stale agent command was accepted'
 [MediaTest]::Key($window,0x21)
 $s = Wait-State {param($s) $s.index -eq 3 -and $s.ready}
 Check ($s.kind -eq 'audio' -and $s.width -eq 0) 'Audio view failed'
 Tool media_pause | Out-Null; Tool media_seek_absolute @{seconds=6} | Out-Null
 $s = Wait-State {param($s) $s.paused -and [Math]::Abs($s.position-6) -lt 0.1}
 Check ($s.duration -eq 12) 'Audio seeking failed'
 [MediaTest]::Key($window,0x24)
 $s = Wait-State {param($s) $s.index -eq 0 -and $s.ready}
 Check ($s.kind -eq 'image') 'Home failed'
 $mcp = '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"media_get_state","arguments":{}}}' | & "$workspace\build\aiview-tool.exe" --pipe "aiview-$($process.Id)" --mcp | ConvertFrom-Json
 Check ($mcp.result.isError -eq $false -and $mcp.result.content[0].type -eq 'text') 'MCP tool invocation failed'
 $extended = Join-Path $fixture 'extended'
 [IO.Directory]::CreateDirectory($extended) | Out-Null
 FF -f lavfi -i 'testsrc2=size=640x360:rate=24' -t 3 '-c:v' libx265 -x265-params 'pools=2:log-level=error' -y "$extended\hevc.mp4"
 Tool media_open @{path="$extended\hevc.mp4"} | Out-Null
 $s = Wait-State {param($s) $s.ready -and $s.path -match 'hevc.mp4' -and $s.position -gt 0.2}
 Check ($s.width -eq 640 -and $s.duration -eq 3) 'HEVC decoding failed'
 FF -f lavfi -i 'testsrc2=size=320x240:rate=24' -t 4 -vf "select='if(lt(n,24),not(mod(n,2)),not(mod(n,3)))'" -vsync vfr '-c:v' libx264 -y "$extended\vfr.mp4"
 $oracle = (& ffprobe -v error -select_streams v:0 -show_entries frame=best_effort_timestamp_time -of json "$extended\vfr.mp4" | ConvertFrom-Json).frames
 Tool media_open @{path="$extended\vfr.mp4"} | Out-Null
 $null = Wait-State {param($s) $s.ready -and $s.path -match 'vfr.mp4'}
 Tool media_pause | Out-Null; Tool media_seek_absolute @{seconds=0} | Out-Null
 $null = Wait-State {param($s) $s.paused -and $s.position -lt 0.005 -and -not $s.seeking}
 foreach ($index in 1..15) {
  Tool media_step_frame @{direction=1} | Out-Null
  $target = [double]$oracle[$index].best_effort_timestamp_time
  $s = Wait-State {param($s) [Math]::Abs($s.position-$target) -lt 0.002 -and $s.paused}
 }
 Check ($s.position -gt 1) 'VFR frame stepping did not cross the cadence change'
 foreach ($index in 14..10) {
  Tool media_step_frame @{direction=-1} | Out-Null
  $target = [double]$oracle[$index].best_effort_timestamp_time
  $s = Wait-State {param($s) [Math]::Abs($s.position-$target) -lt 0.002 -and $s.paused}
 }
 Check ($s.position -lt 1) 'VFR backward stepping did not cross the cadence change'
 $images = @'
from PIL import Image
import sys
root=sys.argv[1]
image=Image.new('RGB',(80,40),'red'); exif=image.getexif(); exif[274]=6
image.save(root+'/rotated.jpg',exif=exif)
image=Image.new('RGBA',(120,80),(0,255,0,0))
for x in range(60):
 for y in range(80): image.putpixel((x,y),(0,255,0,255))
image.save(root+'/transparent.png')
import random
Image.frombytes('RGB',(768,768),random.Random(42).randbytes(768*768*3)).save(root+'/noise.png')
'@
 $images | & python - $extended
 if ($LASTEXITCODE) {throw 'Image fixtures require Python with Pillow'}
 Tool media_open @{path="$extended\rotated.jpg"} | Out-Null
 $s = Wait-State {param($s) $s.ready -and $s.path -match 'rotated.jpg'}
 $op = Tool media_copy_frame
 $null = Wait-State {param($s) $s.last_operation.id -eq $op.operation -and $s.last_operation.status -eq 'completed'}
 $pixels = [MediaTest]::Clipboard("$fixture\clipboard-rotated.bmp")
 Check ($pixels[0] -eq 40 -and $pixels[1] -eq 80) 'EXIF rotation missing from copied image'
 Tool media_open @{path="$extended\transparent.png"} | Out-Null
 $s = Wait-State {param($s) $s.ready -and $s.path -match 'transparent.png'}
 Check ($s.width -eq 120 -and $s.height -eq 80) 'Transparent PNG failed to decode'
 $op = Tool media_copy_frame
 $null = Wait-State {param($s) $s.last_operation.id -eq $op.operation -and $s.last_operation.status -eq 'completed'}
 $pixels = [MediaTest]::Clipboard("$fixture\clipboard-transparent.bmp")
 Check ($pixels[5] -lt 10 -and $pixels[6] -lt 10 -and $pixels[7] -lt 10) 'Transparent area was not composited against black in clipboard fallback'
 $op = Tool media_capture_frame
 $null = Wait-State {param($s) $s.last_operation.id -eq $op.operation -and $s.last_operation.status -eq 'completed'}
 $frame = Tool media_get_frame @{operation=$op.operation}
 [IO.File]::WriteAllBytes("$fixture\agent-transparent.png",[Convert]::FromBase64String($frame.data))
 & python -c "from PIL import Image; import sys; p=Image.open(sys.argv[1]).convert('RGBA'); assert p.getpixel((90,40))[3]==0; assert p.getpixel((30,40))[3]==255" "$fixture\agent-transparent.png"
 Check ($LASTEXITCODE -eq 0) 'Agent PNG lost transparency'
 & "$PSScriptRoot\capture-window.ps1" -ProcessId $process.Id -Output "$fixture\transparent-window.png"
 Tool media_open @{path="$extended\noise.png"} | Out-Null
 $null = Wait-State {param($s) $s.ready -and $s.path -match 'noise.png'}
 $op = Tool media_capture_frame
 $null = Wait-State {param($s) $s.last_operation.id -eq $op.operation -and $s.last_operation.status -eq 'completed'}
 $frame = Tool media_get_frame @{operation=$op.operation}
 Check ($frame.data.Length -gt 1MB -and $frame.width -eq 768) 'Large PNG IPC response failed'
 # New files created after launch do not mutate the old folder snapshot; explicit open resnapshots.
 if ($Ssh) {
  $remote = '/data/devel/aiview-check-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss')
  & ssh ai "mkdir -p $remote"; if ($LASTEXITCODE) {throw 'SSH fixture directory failed'}
  & scp "$fixture\03-video.mp4" "ai:$remote/source.mp4"; if ($LASTEXITCODE) {throw 'SSH fixture upload failed'}
  # Insert a 128 MiB free atom immediately before tail moov; original mdat offsets remain valid.
  $python = @'
import struct,sys
root=sys.argv[1]
data=open(root+'/source.mp4','rb').read(); at=0
while at<len(data):
 size,kind=struct.unpack('>I4s',data[at:at+8])
 if kind==b'moov': break
 at+=size
with open(root+'/tail.mp4','wb') as f:
 f.write(data[:at]); f.write(struct.pack('>I4s',128*1024*1024,b'free')); f.seek(128*1024*1024-8,1); f.write(data[at:])
'@
  $python | & ssh ai "python3 - $remote"; if ($LASTEXITCODE) {throw 'Tail MP4 fixture failed'}
  Tool media_open @{path="ssh://ai$remote/tail.mp4"} | Out-Null
  $s = Wait-State {param($s) $s.ready -and $s.path -match 'tail.mp4' -and $s.position -gt 0.1}
  Check ($s.remote_bytes_read -lt 16MB -and $s.remote_bytes_read -gt 0) 'SSH startup downloaded too much or did not use SFTP'
  Tool media_pause | Out-Null; Tool media_seek_absolute @{seconds=9} | Out-Null
  $s = Wait-State {param($s) $s.paused -and [Math]::Abs($s.position-9) -lt 0.05 -and -not $s.seeking}
  Check ($s.remote_bytes_read -lt 16MB) 'Remote seek downloaded the entire file'
  Tool media_step_frame @{direction=-1} | Out-Null
  $s = Wait-State {param($s) [Math]::Abs($s.position-(9-1.0/24)) -lt 0.005}
  Check $s.paused 'Remote reverse frame failed'
  $s | ConvertTo-Json -Depth 8 | Set-Content "$fixture\ssh-state.json"
 }
 "PASS $script:checks native media checks; fixtures and captured frames: $fixture"
} finally {
 if (-not $process.HasExited) { $process.CloseMainWindow() | Out-Null; if (-not $process.WaitForExit(5000)) {Stop-Process -Id $process.Id} }
}
