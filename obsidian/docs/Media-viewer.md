# Media viewer design

2026-09-22. Implementation plan, not an implemented feature. Working executable name: **aiview**. This supersedes the earlier suggestion to implement the first image viewer inside jptxt. jptxt remains the text/hex viewer and editor.

## Product contract

A compact, keyboard-driven media window opens from aicmd and can also run independently. JPEG/PNG and MP4 are first-class requirements for the first usable release; audio belongs to the same playback model. Avoid a photo-only architecture that later needs replacing for video.

The window starts at the focused file and browses the media in that folder. It has one media surface and a small, transient status overlay showing filename, folder position, playback time or image dimensions, loading/error state. No persistent sidebar, thumbnails, playlist panel or large transport buttons. Retain native window borders initially; F toggles fullscreen. Audio-only playback retains a keyboard-operable window with title and time.

Defaults selected for implementation: video/audio autoplay on opening, images remain until navigation, end of video holds the last frame, no automatic advance or wraparound. Volume is retained within the viewer session; playback position resets when switching files. These are adjustable later, not reasons to block the first release.

### Keyboard contract

| Context / key | Action |
|---|---|
| aicmd: Space on a supported media candidate | Enter media viewer at the focused file; do not toggle its mark |
| aicmd: Space on a directory/nonmedia file | Preserve existing mark behavior |
| aicmd: Insert / Shift+Space | Mark any entry, including media; Insert retains mark-and-advance |
| Viewer: PageDown / PageUp | Next / previous media item in this folder session |
| Viewer: Home / End | First / last media item; these never seek inside the current clip |
| Viewer: Left / Right | Seek audio/video -5 / +5 seconds; preserve playing/paused state; clamp at endpoints |
| Viewer: comma / period | Previous / next video presentation frame; pause and remain paused |
| Viewer: Space | Pause/resume audio/video; no automatic slideshow for still images |
| Viewer: Escape | Close viewer and return to the originating aicmd panel, including from fullscreen |
| Viewer: F | Toggle fullscreen |
| Viewer: + / - / 0 / 1 | Still-image zoom in / out / fit / actual pixels |
| Viewer: M / Ctrl+Up / Ctrl+Down | Mute / volume up / down |

At the first/last item, PageUp/PageDown do nothing and briefly show the boundary. A failed file displays its name/error and remains navigable. A new file starts playing even if the previous one was paused; Space then pauses it. Frame stepping is disabled for audio/stills with a brief explanatory overlay. Keyboard repeat for folder/seek operations is coalesced to the newest requested state; individual frame steps are serialized through a small bounded queue, never silently converted into a nominal-FPS time jump.

Comma/period must use the intended punctuation key mapping on the user's Windows keyboard, including Japanese layouts. Define and test the binding independently of shifted < and >. Never let backend defaults interpret Home/End or PageUp/PageDown differently.

## Backend choice

Use **libmpv**, isolated in the player executable, with an original C++17/native host. Its media stack provides decoding, audio/video synchronization and playback controls. FFmpeg command-line tools are useful for test fixture generation and probing, but implementing the player directly on FFmpeg would also require owning timing, audio output, rendering, seeking and decoder draining.

| Option | Assessment |
|---|---|
| libmpv in a separate aiview process | Recommended production boundary: typed commands/events, provider-backed input, custom compact UI, commander independent of codec runtime |
| mpv executable controlled by JSON IPC | Good short feasibility probe; less control over provider callbacks and window/input ownership. Not the final remote-stream architecture |
| FFmpeg libraries directly | Maximum control, substantially more playback infrastructure; reserve for a specific capability libmpv cannot meet |
| Windows-only imaging/playback APIs | Native integration, but separate image/video behavior and less shared implementation across platforms |

Initial Windows rendering plan: libmpv render API with a native window/OpenGL surface. Establish hardware decode/interoperability during the first spike. Keep rendering behind a platform adapter; do not scatter graphics context handles into the session/controller. A Windows child-window video output is an alternate spike if the render path misses performance requirements, not a cross-platform assumption. macOS/Linux receive native adapters later.

Images use the same media session and initially the same backend. JPEG EXIF orientation, PNG transparency, aspect ratio and color behavior must pass the fixture checks. Add a narrow still-image renderer only if testing demonstrates a concrete shortcoming. Do not build a public codec plugin loader before this boundary is proven.

Pin the tested libmpv runtime, client API and build options. The initial codec acceptance target is JPEG, PNG, MP4/H.264/AAC, MP3 and WAV; also test MP4/HEVC as a priority because an MP4 extension does not identify its codec. Unsupported codecs fail per file. GPU decode is requested with software fallback and a diagnostic showing which path is active. HDR, advanced color grading, animated formats and subtitle UI are later work; never claim HDR fidelity from an SDR playback test.

Packaging remains separate: aicmd must still run without the optional player/runtime. Missing backend means an actionable install/configure message. Package provenance, checksums, notices and the exact dependency build/license manifest belong to the player distribution. mpv's upstream licensing varies with build configuration, as does FFmpeg's; verify the actual shipped build instead of assuming any binary is LGPL or that process separation resolves licensing obligations.

## Components and ownership

Keep this in the aicmd repository initially, as an optional CMake target; it can move to its own repository later without changing the integration protocol.

| Component | Owns |
|---|---|
| Shared filesystem/types targets | Location, local/SFTP byte sources, source handles, bounded reads, cancellation and errors |
| media_core | Folder session, item order/current ID, playback intent, command validation, generations and state/events; no GUI or mpv types |
| mpv_backend | libmpv instance, asynchronous commands, observed properties, decode/playback events |
| platform shell | Window, input, fullscreen, render surface, overlay, wakeups; Win32 first |
| aicmd viewer adapter | Open request, session attachment, current-item/close events, panel focus return |

Refactor the current filesystem implementation into a separately linkable target when needed. Do not instantiate a full two-panel Core merely to read media. Both programs reuse the provider code; the player owns its own dedicated media connection. This intentionally survives commander closure in standalone mode and keeps playback traffic separate from commander copies/listings. Reusing the same SSH process across executables is not an initial requirement.

Runtime model: UI, backend events/rendering, and provider I/O have separate execution responsibilities. No directory scan, network request or image decode on the UI callback. Backend wakeups post messages; callbacks must not synchronously invoke UI work or acquire locks that UI code holds while waiting for libmpv. Source cancellation and shutdown wake blocked readers and are tested before adding embedding.

## Folder-session rules

When launched by aicmd, consume a **snapshot of its displayed order**, including its sort and hidden-file policy. Build the candidate list from files in that directory, ignoring marks; no recursion and no scanning the entire drive. Start at the focused candidate. Standalone mode enumerates the parent folder with the same shared sorting rules.

Use extension/capability hints to find candidates without probing every remote file. Confirm actual content only when opening. Unknown extensions are omitted from automatic folder browsing initially; explicit standalone open can attempt them. Corrupt or unsupported candidates remain in the list with a visible error, so indexes are predictable.

Each item carries an opaque ID, Location and available metadata. The folder list is stable during a viewing session; aicmd refreshes and remote changes do not silently reorder it. A future explicit rescan can rebuild while retaining current identity. Deleted files produce a recoverable item error. Large lists use bounded/paged transfer over IPC, not a giant command line or an OS-dependent argument limit.

On close, return current item ID and origin session/panel/tab tokens. If that tab still exists and still shows the same directory, focus the viewed item if present, preserving marks. Otherwise leave the current commander location alone. Restore keyboard input to the list, not only the top-level window. Do not steal foreground from an unrelated app. Player crash follows the same input-recovery path.

## Seeking and frame stepping

The controller owns intended playback state. +/-5-second requests target media time, including while paused. Coalesce repeated seeks and ignore completions from replaced loads. Frame stepping pauses first and uses backend frame-step/back-step operations; never calculate a frame as 1/nominal FPS. Test by presentation timestamps and known numbered frames, including variable-frame-rate and B-frame clips.

Reverse stepping can require decoding from a preceding keyframe. A bounded recently-decoded-frame cache may improve it later, but should not delay the initial validated backend approach. Do not promise constant-time reverse stepping or exactness for every damaged/timing-filtered stream. A source that cannot seek reports that limitation rather than pretending to step.

Use a state model such as Closed, Opening, ReadyImage, Playing, Paused, Seeking, Buffering, Ended, Error. Actual backend state and user intent are distinct. Every load has a generation token; late metadata/frames/errors from the previous item cannot replace the current item. Switching items cancels obsolete prefetch and stops old audio promptly.

## SSH media: seekable input is required

Do not route large video through today's full-file jptxt staging path or its 1 GiB admission cap. Implement a seekable ByteSource around the existing persistent SFTP transport:

```
open_read(Location) -> handle, size, source_version, capabilities
read_at(handle, offset, length, cancellation) -> bytes
close(handle)
```

Version information includes available size/mtime and connection identity; acknowledge that these are not cryptographic content identities. Feed a custom libmpv input scheme through its stream callbacks. Seek updates the requested offset; random reads can reach the tail immediately. MP4 files with the moov index at the end are a required test, so sequential stdin streaming is insufficient.

Proposed initial budgets: 1 MiB cache blocks, 64 MiB RAM media cache, at most four blocks of forward read-ahead, pipelined SFTP requests within each block, two adjacent still-image prefetch candidates with a shared decoded-image budget. These are starting limits to measure, not benchmark results. Avoid eager decoding/prefetch of neighboring videos. Keep current reads higher priority than prefetch; a far seek invalidates obsolete requests. Disk cache is optional and bounded; do not present holes in a sparse local file as downloaded zero bytes.

Callbacks may wait on the provider worker from the demux thread, never from the UI/render thread. Cancellation must unblock pending reads. Each stream/seek generation owns its outstanding requests so cancelling an old item cannot kill the new item's transport. Cap allocation/request sizes and decoder work independently of advertised remote metadata.

On disconnect: show reconnect/error state, retain position and intent, then revalidate source metadata before resuming reads. Changed content invalidates cached blocks. Unknown-length/nonseekable sources cannot claim full folder-review seek behavior. First support regular seekable local/SFTP files; livestreams are outside this milestone.

## Integration and model commands

Use a private versioned local IPC channel: named pipe on Windows, Unix-domain socket on POSIX, restricted to the user/session. Exchange structured messages with request IDs and bounded sizes. Pass opaque capabilities/Locations, never shell fragments or SSH key material. Launch by literal OS argument vectors; Unicode/spaces and an already-running viewer are mandatory cases.

Initial one viewer session per attached commander window; opening again replaces its folder session. Standalone launches can create independent sessions. After commander exit, an already-open viewer continues with its snapshot and provider. Optional future embedded presentation reuses media_core/backend; do not embed another process's top-level window as the long-term cross-platform API.

Typed commands, shared by keyboard and eventual model adapters:

- media_open, media_close, media_get_state, media_get_capabilities
- media_next, media_previous, media_first, media_last, media_select
- media_play, media_pause, media_seek_relative, media_seek_absolute, media_step_frame
- media_set_volume, media_set_mute, media_set_fullscreen, media_set_image_view

Commands validate session/item generation and capabilities. Long operations return accepted/request IDs; events distinguish completion/error from mere acceptance. Throttle progress/state events, preserve command results and error events. Screen/image capture and destructive media edits are separate future capabilities, not implicit playback commands.

## Delivery sequence and acceptance gates

1. **Backend spike:** pin a candidate libmpv build; open JPEG, transparent PNG, H.264/AAC MP4, HEVC MP4, MP3/WAV; exercise seek and both frame directions; check render-thread rules, GPU/software paths and runtime footprint. Produce a runnable probe and record failures before settling the adapter. No user media needs uploading.
2. **Standalone local viewer:** mixed-folder navigation and the complete requested key map, compact window, still-image fit/zoom, autoplay/end behavior, errors and rapid file switching. Video is part of this gate, not postponed behind image polish.
3. **Commander integration:** Space dispatch, Insert/Shift+Space marking, bounded snapshot IPC, reusable process, current-item return and keyboard recovery. jptxt text behavior remains intact. F3 may dispatch recognized media to aiview once this routing is tested; the explicit required entry key is Space.
4. **Seekable SSH playback:** shared read-at provider, cancellation/generation handling, bounded read cache, stream callbacks, remote images and MP4 seeking/frame stepping. This gate must pass before declaring media support complete for the existing SSH workflow.
5. **Hardening and portability:** deployment/package manifest, soak/resource tests, Linux host via ssh ai and later native macOS shell; then assess whether embedding adds value. Public plugins follow actual renderer/provider needs.

Acceptance matrix: mixed JPEG/PNG/video/audio order; first/last boundaries; Unicode/spaces; transparent PNG; rotated JPEG; portrait video; zero-length/corrupt file; unsupported codec; 1080p/4K; VFR and B-frames; paused and playing 5-second seeks; repeated backward frames; EOF; no audio bleed after switching; folder changed/deleted during review; huge folder; viewer close/crash and path-editor input recovery; SSH MP4 index at front and tail; latency, interrupted reads, changed source, rapid seek/cancel, bounded memory and no full-file startup download.

Generate deterministic, numbered-frame CFR/VFR fixtures with FFmpeg and use their timestamps as the oracle. Test actual rendered content, audio playback and GPU path in addition to state transitions. Measure cold/warm first image/frame, folder-switch latency, +/-5-second seek latency, reverse-step latency, memory, bytes fetched and cache hit rate. Record machine/network/codec details; establish realistic thresholds from the spike rather than inventing universal millisecond promises.

## References checked

- [mpv stable manual](https://mpv.io/manual/stable/): playback commands, image lifetime and stepping semantics; reverse stepping has seek/timing limitations.
- [libmpv examples](https://github.com/mpv-player/mpv-examples/blob/master/libmpv/README.md) and [render API](https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h): embedding and rendering integration.
- [Read-only stream callbacks](https://github.com/mpv-player/mpv/blob/master/include/mpv/stream_cb.h): custom seekable input contract.
- [mpv copyright/build licensing](https://github.com/mpv-player/mpv/blob/master/Copyright) and [FFmpeg distribution guidance](https://ffmpeg.org/legal.html): verify the selected runtime/build before packaging.
