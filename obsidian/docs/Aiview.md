# aiview: running, controls and agent tools

2026-09-22. First Windows implementation; see [design](Media-viewer.md) for longer-term goals. The viewer is a separate C++17 executable in this repository. jptxt remains the text/hex viewer and editor.

## Run

```powershell
.\scripts\setup-aiview.ps1
.\build.bat
.\build\aiview.exe -- 'C:\media\clip.mp4'
.\build\aiview.exe -- 'ssh://ai/data/devel/media/clip.mp4'
```

The setup script downloads a pinned, checksum-checked third-party libmpv runtime. It needs 7-Zip. The DLL is already installed in this workspace's `build/`; no install is needed here. Runtime provenance and binary-distribution limitations: [dependency notes](../../third_party/mpv/README.md). Aicmd works without the optional runtime. Missing libmpv gives an actionable viewer error. CMake option `AICMD_BUILD_AIVIEW=OFF` disables the native viewer/tool executables.

In aicmd, **Space** on a recognized media file opens aiview. **Insert** and **Shift+Space** still mark media. Space on other entries keeps its original marking behavior. F3/F4 retain their existing jptxt contract; this release does not reroute F3.

One viewer can be open per commander window. Close it with Escape before opening a different folder from that commander. Standalone launches are independent. The player remains usable if commander exits.

## Viewer keys

| Key | Action |
|---|---|
| PgUp / PgDn | Previous / next media item |
| Home / End | First / last folder media item |
| Left / Right | Seek -5 / +5 seconds, preserving pause |
| , / . | Previous / next presentation frame; stays paused |
| Space | Pause/resume video/audio |
| Ctrl+C / Ctrl+Insert | Copy decoded image/current video frame to Windows clipboard |
| Escape | Close, even in fullscreen |
| F | Fullscreen toggle |
| + / - / 0 / 1 | Still-image zoom / fit / actual pixel size |
| M / Ctrl+Up / Ctrl+Down | Mute / volume +/-5 |

There are no persistent transport controls or side panels. Filename, folder index, time/pause and errors appear in the native title bar. Images hold indefinitely. Video/audio autoplay; EOF holds, without advancing to another item. Errors remain navigable. A new item resets position and starts playback. Volume survives item changes.

Clipboard copies are full decoded resolution (up to a 64-megapixel capture budget), excluding window borders, letterboxing, overlays and controls. Windows receives CF_DIBV5 with alpha plus a black-composited CF_DIB fallback for applications without alpha support. This is decoded SDR imagery, not the original file/EXIF data or an HDR archival export. Clipboard contention produces an error instead of claiming success. For an exact chosen video frame, pause/step before copying. Cmd+C is reserved for a future macOS shell; there is no macOS viewer yet.

## Folder and SSH behavior

Commander sends a frozen snapshot in its displayed order, including its active sort/hidden policy and ignoring marks. Launch uses an exclusive temporary JSON snapshot (32 MiB / 100,000 candidates maximum), not a command line full of filenames. A return file carries the final location. Commander checks original panel/tab/directory before applying it, preserves marks and restores list keyboard navigation. Temporary files are removed when commander collects viewer exit. If commander is closed first, its small files can remain in `%TEMP%\aicmd-media`.

Standalone opening enumerates the parent directory asynchronously, name-sorts and filters hidden entries, while retaining an explicitly requested file. Candidate extensions: JPEG/PNG/BMP/WebP; MP4/M4V/MOV/MKV/AVI/WebM/MTS; MP3/WAV/FLAC/M4A/OGG/Opus/AAC. Content is decoded only when opened. These hints are not a guarantee that every codec or animated format is validated.

SSH uses the existing OpenSSH config/authentication policy and SFTP transport. Each media source owns its connection and supports arbitrary byte offsets. A 64 MiB LRU caches 512 KiB blocks; libmpv has separate bounded demux buffers. Old stream generations are cancelled, including blocked reads, independently of commander browsing/copies. No full-file staging or 1 GiB staging cap applies. Remote startup and tail-index seeks have been tested with a sparse MP4 containing a 128 MiB free atom: less than 2 MiB of remote bytes were read for playback, seeking and reverse stepping. This is a functional random-access test on the current LAN, not a WAN throughput benchmark.

Automatic reconnect, mid-stream remote-content version checking, disk cache, adjacent-image prefetch and cross-process SSH connection sharing are not implemented. After a disconnected/changed source fails, explicitly reopen it. Reverse frame stepping may decode from a previous keyframe and is not constant-time.

## Agent interface

No Gemini SDK or model types enter the controller. Keyboard and tools dispatch the same typed commands. `aiview-tool.exe` is a local adapter usable by Gemini-like agents, other function-call hosts, or an MCP client. It does not itself connect to a model or upload media.

Every viewer exposes `\\.\pipe\aiview-PID`, restricted to its Windows user and local clients. Requests are JSON lines; request limit 1 MiB, response limit 4 MiB. A client exchanges one request per pipe connection. There is no TCP listener. State includes PID/pipe name, current path/index/generation, playback readiness/time, decoder, remote bytes fetched, errors and the last capture operation.

```powershell
# Use the PID of the viewer being controlled.
'{"name":"media_get_state"}' | .\build\aiview-tool.exe --pipe aiview-12345
'{"name":"media_seek_relative","arguments":{"seconds":5,"generation":1}}' |
    .\build\aiview-tool.exe --pipe aiview-12345
.\build\aiview-tool.exe --tools
.\build\aiview-tool.exe --pipe aiview-12345 --mcp
```

`--tools` returns tool names, descriptions and JSON input schemas. A function-call adapter can map these into its model provider's declarations and forward calls as `{name, arguments}`. The MCP stdio adapter implements initialization (protocol 2024-11-05), ping, tools/list and tools/call. Configure the client to launch the absolute `aiview-tool.exe` path with `--pipe aiview-PID --mcp`. Live Gemini service integration has not been tested.

Available tools:

- `media_open`, `media_close`, `media_get_state`, `media_get_capabilities`, `media_get_items` (128-item pages).
- `media_next`, `media_previous`, `media_first`, `media_last`, `media_select` (zero-based index).
- `media_play`, `media_pause`, `media_seek_relative`, `media_seek_absolute`, `media_step_frame` (direction -1 or +1).
- `media_set_volume` (0..100), `media_set_mute`, `media_set_fullscreen` (0/1), `media_set_image_view` (log2 zoom; 0 fits).
- `media_copy_frame` writes the full-resolution clipboard image.
- `media_capture_frame` captures for agent inspection **without changing the clipboard**; `media_get_frame` retrieves the completed operation as PNG. Its longest side is at most 768 pixels, with SDR color and preserved alpha. MCP returns actual image content plus metadata; raw JSON returns base64. Capturing and retrieving are explicit actions, never background uploads.

Playback commands return acceptance; poll state for actual position/readiness/error. Capture tools return an operation ID. Poll `last_operation` until that ID is `completed` or `failed`, then retrieve it if using agent capture. Only the last agent image is retained. It is rejected after an item-generation change. Its `position_requested` is the position observed when capture was requested, not a claim of exact PTS during advancing playback. Pause before capture when precision matters.

Mutating item commands accept an optional expected `generation`; agents should supply the value from state to reject stale operations. Folder opens replace the session asynchronously. Command queues and capture concurrency are bounded; a busy capture or full queue returns an error. Capture errors never silently replace clipboard contents with another item's frame.

## Validation and remaining work

Reproduce: `build.bat`, `scripts/test-media.ps1 -Ssh`, `scripts/test-media-commander.ps1`. Media fixtures use FFmpeg/ffprobe and Python/Pillow; generated files/screenshots stay in ignored artifacts. Linux shared-core checks use `scripts/test-linux.ps1` over `ssh ai`; no WSL.

Verified on this Windows machine: JPEG, PNG, H.264/AAC MP4 with D3D11 hardware decode, HEVC MP4, WAV playback state/seeking, both CFR/VFR frame directions against FFprobe timestamps, EXIF rotation in clipboard pixels, corrupt-file recovery, clipboard key dispatch/dimensions/content, native folder navigation, MCP invocation/image responses, and SFTP random-access playback. Actual frame and window captures were inspected. Audio device quality/listening, HDR fidelity, 4K/long-session stress, all keyboard layouts and DPI settings remain separate acceptance work.

Current implementation choices differ from the target architecture in three explicit ways: native Windows `wid`/D3D11 instead of the portable OpenGL render API; bounded temporary snapshot files instead of paged commander-session IPC; close-before-reopen rather than replacing an already-attached viewer session. These are behind the player boundary and do not put GUI/model dependencies into the shared controller.

Next gates: binary license/build manifest and packaging, long-run/large-media performance and interruption tests, reusable commander attachment, then Linux/macOS native adapters. Public plugin ABI remains deferred until the backend/provider needs are proven.

API references: [mpv commands/client behavior](https://mpv.io/manual/stable/), [MCP tools](https://modelcontextprotocol.io/specification/2024-11-05/server/tools).
