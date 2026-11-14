# M1: compact Windows directory browsing

## Archives

- [x] Read-only ZIP/ZIP64/TAR/TGZ browsing, F3/F4 staging, F5 extraction, local + SSH (2026-11-15).
- [ ] Packing/writing (Alt+F5, F5 into archive, delete inside); 64-bit WCX plugin host; 7z/RAR/XZ/BZip2 via plugins or bundled tar.exe.
- [ ] Faster inflate; archives inside archives; search and media playback inside archives.

## Search

- [x] Alt+F7 lanes, streaming result tab, go-to-file, CLI search, local + SFTP (2026-11-14).
- [ ] Tune lane count/caps on cold cache, SMB and WAN; date/size filters; search inside archives; regex.

## Release readiness (v0.2.0)

- [x] Application icon.

- [x] Version resource, F1 help/About, packaged zip with smoke test (`scripts/package.ps1`).
- [x] Optional jptxt and libmpv with graceful fallbacks.
- [ ] Choose project LICENSE (blocks public release); decide jptxt/libmpv bundling.
- [ ] Interactive check of no-jptxt / no-libmpv fallbacks on a clean machine.
- [ ] Code signing, installer or winget manifest; GitHub release upload (requires explicit go-ahead).

- [x] Consistent full-row mouse cursor after keyboard navigation; plain clicks preserve marks, Ctrl/Shift clicks mark explicitly; native regression coverage (2026-09-27).

First usable implementation is built and validated. Broader acceptance work remains below. [Design](../docs/Design.md), [status](../status/Status.md).

## Implemented

- [x] C++17 shared core and CMake; Windows GUI and headless CLI.
- [x] Background directory enumeration, stale-result rejection, cancellation, bounded queues/listings.
- [x] Two independent file panels and tabs; navigation history, parent, refresh, hidden toggle, sort.
- [x] Compact full-width lists with native tabs and inline path editing. No tree/sidebar/toolbar buttons.
- [x] Keyboard panel/tab navigation, mark with Insert/Space, focus movement, F4 editor launch.
- [x] Virtual list rows, Unicode/native paths, long-path adapter, directory links identified.
- [x] Session JSON, backup, corrupt-state preservation, per-session file lock.
- [x] Local Git repository; ignore generated artifacts; structured Obsidian vault.

## Verification / remaining

- [x] Re-run Windows native smoke after compact UI revision and inspect screenshot.
- [x] Linux core/CLI build and tests on `ssh ai` under `/data/devel/` (no WSL).
- [x] Record final binary sizes and performance results in status.
- [ ] Verify actual 150%/200% DPI displays and accessibility with a screen reader.
- [ ] Real offline/permission-denied UNC share testing; fake-provider denial is covered.
- [x] Automate keyboard routes for panel switching, new/close tab, path commit/cancel, and marking through native controls.
- [ ] Extend GUI automation to tab scroll/focus restoration and physical keyboard input.
- [ ] Verify editor opening with an already-running jptxt instance and difficult filename quoting.
- [ ] Improve per-tab history focus/scroll restoration, beyond current per-tab state.
- [ ] Add partial listing batches; current list publishes when sorted enumeration completes.
- [ ] Resolve potentially blocked network syscall at application shutdown.

## Next milestones

- [ ] M2: generated schemas, MCP stdio, authenticated attachment to existing GUI session.
- [ ] M3: terminal frontend usable over SSH; keyboard parity with GUI.
- [ ] M4: macOS / Linux GUI frontends.
- [x] M5a: move/rename/mkdir/Recycle Bin/permanent delete, local and SSH, no-overwrite (2026-11-14).
- [ ] M5b: explicit conflict prompts (skip/rename/overwrite), per-item partial completion report, attribute/timestamp preservation.
- [x] Explicit read-only jptxt viewer contract for F3; F4 enables editing in place.
- [ ] M6: Gemini function/Live adapter over the shared commands.
- [x] Configure GitHub remote and publish main with id_ghjuricap.

- [ ] Versioned viewer plugin API after aiview's backend/provider boundaries are proven (initial media viewer is separate from jptxt).
- [ ] Provider location refactor, SFTP first, FTP/FTPS and SMB/CIFS adapters; see plugin design.

- [x] Full-row cursor with red selection marks, Ctrl+D persistent bookmarks, Alt+F1/F2 drive menus.

## SSH increment

- [x] Remove cursor-triggered layout/full-list redraw; native regression check.
- [x] SSH config alias discovery and Alt+F1/F2 SSH menus.
- [x] Typed remote locations, SFTP browsing, persisted remote tabs/bookmarks.
- [x] Persistent connections, pipelined F3 staging and non-overwriting F5 file/directory copying.
- [x] Live ai roundtrip, cancellation, cache and native GUI checks.
- [ ] Remote edit/save conflict workflow, overwrite/merge/resume policies.
- [ ] Range-based remote viewing and automatic cache lifecycle cleanup.
- [ ] Live Linux transport and macOS validation, WAN throughput profiling.

- [x] Restore keyboard navigation after external viewer exit; verify local/SSH cursor movement and preserve path-editor/tab input.

## Media viewer

See [full design](../docs/Media-viewer.md).

- [x] Specify standalone player architecture, keys and SSH streaming contract.
- [x] Backend spike: pinned libmpv, JPEG/PNG + MP4/audio, seeks/frame steps, native D3D11 rendering and hardware decode.
- [x] Standalone local mixed-folder viewer with requested keyboard controls.
- [x] Space launch and bounded folder snapshots; preserve mark commands, current-item return and keyboard navigation.
- [x] Provider read-at API and bounded, seekable SSH playback.
- [x] Ctrl+C/Ctrl+Insert copies decoded image/frame; preserve alpha with a compatible clipboard fallback.
- [x] Local tool schemas/MCP, generation checks, asynchronous capture status and explicit agent PNG capture.
- [x] Native JPEG/PNG/HEVC/H.264/WAV, CFR/VFR, clipboard, MCP and SFTP tail-index fixtures.
- [ ] Reusable commander attachment and paged session IPC, replacing temporary snapshot files.
- [ ] Remote reconnect/version checks and interruption/latency/resource soak tests.
- [ ] 4K/HDR, mirrored EXIF orientations, all keyboard layouts/DPI and audio-device quality validation.
- [ ] Redistributable runtime license/build manifest and packaging; native Linux/macOS shells.

Implemented behavior and agent examples: [aiview](../docs/Aiview.md).

## Copy queue

- [x] Dedicated bounded FIFO, frozen selection/destination and continued browsing.
- [x] Byte/root progress, active and queued cancellation, compact F5/Escape/Shift+Escape UI.
- [x] Windows/Linux queue regressions, live SSH queued uploads, native large-folder progress/cancellation.
- [ ] Durable incremental item journal and deferred nonmodal conflict inbox; continue unaffected items.
- [ ] Revision-checked skip/rename/replace/merge decisions shared with model adapters.
- [ ] Crash recovery, detach/reconnect and opt-in unattended notifications.
- [ ] WAN/resource/long-duration stress, fairness and bandwidth tuning.

See [copy jobs design](../docs/Copy-jobs.md).
