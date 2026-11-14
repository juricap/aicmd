# Development log

## 2026-11-15: archives as folders

Researched TC behaviour (Enter/Ctrl+PgDn, `..` leaves, F3 temp view, F5 unpack, WCX plugins). Chose path-transparent archive locations resolved inside the filesystem provider, so tabs, history, sessions and Ctrl+L need no new location type. Wrote inflate/gzip/ZIP/TAR readers over the existing ReadSource (local or SFTP random access). First inflate version was 8× slower than bsdtar on 200 MB; a flat history buffer with memcpy for non-overlapping matches and direct extraction into the destination's staging file brought it to ~3×. GCC -Wextra caught aggregate initialisers and a misleading one-line loop.

## 2026-11-14: search lanes and icon

Added streaming enumeration to providers and a search engine with probing fast lanes, a slow lane and a content lane (design and numbers in docs/Search.md). Core search tabs publish results progressively with a size-proportional interval. During visual verification a global SendKeys call typed into another application because aicmd was not foreground; GUI checks now only post messages to aicmd's own window handles. Generated an icon with a reproducible script.

## 2026-11-14: release packaging

Bumped to 0.2.0. Generated version header feeds both C++ and RC VERSIONINFO. Added Help menu/F1, optional-dependency fallbacks, and package.ps1. The package script's version check caught aiview-tool.exe missing its resource, and PowerShell 5.1 treating VsDevCmd stderr as fatal; both fixed. The packaged executable passes the native smoke from its staged folder.

## 2026-11-14: move, rename, mkdir and delete

Added filesystem primitives shared by local and SFTP locations: name validation, mkdir, no-replace rename, recursive delete that never follows links/junctions (clears read-only attributes after confirmation), Windows Recycle Bin via SHFileOperation with network/removable drives refused rather than silently purged, and move as rename-or-copy-then-delete. Core gained four commands and a JobKind on jobs; completed write jobs refresh visible affected panels in the core instead of the GUI. GUI: F6, Shift+F6, F7, F8/Delete, Shift+F8/Shift+Delete, a centered input dialog with extension-preserving selection, and per-operation progress labels. A test bug (stale snapshot after an auto-refresh) and a PowerShell re-encoding mishap were caught during development. New file-ops suite, live SSH additions and four new GUI smoke stages pass on Windows; Linux suites pass on ai.

## 2026-10-01: queued copies with progress and cancellation

Separated FIFO copies onto one dedicated worker, leaving two workers for scans/views. Submission captures source marks and destination; queue admission bounds waiting jobs and retained source descriptors. Added Queued/Cancelling states, root/byte progress and worker-acknowledged cancellation. Cancelled waiting jobs leave the queue immediately. F5 accepts subsequent batches, Escape cancels the displayed job, and Shift+Escape cancels window transfers. Destination refresh respects subsequent navigation; latest failure survives later successful jobs.

Added local queue tests for 600 selected files, destination capture, FIFO/conflict continuation and cancellation; live SSH tests for queued binary uploads and queued cancellation; native progress/queue/cancel test with 2,000 nested files. The GUI harness attaches its input queue to set deterministic modifier state before posting Escape (desktop synthetic input alone did not reliably deliver Shift). Windows/Linux suites, live SSH and native regressions pass. The copy spec distinguishes implemented behavior from deferred conflict resolution, persistent jobs, notifications and model-facing commands.

## 2026-09-27: consistent mouse input after keyboard navigation

The report list used custom full-row cursor painting and explicit keyboard navigation, while mouse input still followed native selection and label hit-testing. Added a list subclass owning left-button down/up/double-click, full-width row hit detection and explicit cursor/mark semantics. Plain clicks preserve marks; Ctrl toggles and Shift adds a range; double-click navigates the clicked entry. This also avoids native selection/drag tracking for the intercepted left-button sequence. No persistent mouse capture is introduced.

Expanded in-process native smoke to alternate keys with filename/metadata/row-tail clicks, check mark preservation and panel activation, and complete a real double-click directory change. Build and all Windows suites pass; native GUI and aiview/commander return regressions pass. Updated shortcut documentation.

## 2026-09-21

- Studied Double Commander source and local jptxt conventions; wrote shared-core/native-frontend design.
- Built C++17 core, Windows filesystem provider, Win32 GUI, CLI, persistence, and regression tests.
- Fixed long-path fixture setup and session JSON indentation after tests exposed both issues.
- User clarified compact Total Commander / mc layout: removed tree/sidebar/buttons and added dense columns and keyboard marking.
- User specified Linux testing through `ssh ai` under `/data/devel/`; WSL is prohibited going forward.
- Initialized local Git and reorganized the vault into docs, todos, and status. GitHub remote is pending.
- Verified revised full-width UI visually and by native smoke, including keyboard routing and persistence.
- Built the headless core/CLI on `ssh ai` in `/data/devel/aicmd-check-20260921`; aligned POSIX ENOTDIR with Windows `invalid_path` errors. Windows and Linux pass 43 checks.
- Measured final Windows build and 10k/100k directory fixtures; results and remaining gaps are in Status.
- Initial local source checkpoint includes code, tests, scripts, dependency license, compact UI reference, and vault. Use `git log --oneline` for commit IDs.

See [Status](Status.md) for verified results and [M1 todos](../todos/M1-Windows.md) for open work.

## 2026-09-21: viewer and selection

Published main to juricap/aicmd using id_ghjuricap. Added F3 protocol negotiation with sibling jptxt, red selection text, wildcard Num+/Num-, Num* and Ctrl+A. Fixed focus return from selection dialogs, found by native smoke. Windows and Linux shared core pass 54 checks. Documented image plugins and remote provider contracts.

Sibling jptxt viewer implementation committed locally as 132e5df (not pushed). Actual aicmd F3 launch with a spaced filename passed; screenshot confirmed red marked text and an independent cursor. Reproduce with scripts/test-viewer.ps1.

## 2026-09-21: full-row cursor and folder navigation

User clarified that the cursor should highlight the full row. Implemented dark full-row focus with independent red marks, Ctrl+D folder hotlist with add/remove, and Alt+F1/F2 Windows drive selection. Bookmarks persist and remain available when starting with explicit paths. Verified 58 Windows/Linux core checks, native popup-menu routes and selection screenshot.

## 2026-09-22: SSH browsing and transfers; cursor redraw fix

Removed relayout/unchanged-control repaint from cursor movement and limited row invalidation. Added Location boundaries, OpenSSH-backed SFTP v3, config/Include discovery, persistent browse/read/write lanes, bounded pipelined transfer, F3 caching and F5 recursive copy with exclusive staging/publication. Remote keys remain with OpenSSH. Windows/Linux offline suites pass; live ai tests cover byte-exact Unicode transfers, non-overwrite, cancellation and reconnection. Native GUI F3/F5 passed. See SSH documentation for intentionally unimplemented remote save, overwrites, metadata preservation and cache cleanup.

## 2026-09-22: keyboard routing after closing the viewer

Fixed the active-window/child-input mismatch after returning from jptxt. Remember the panel control on deactivation, restore it when the top-level window receives focus, and recover navigation routing if focus lands on the main window. Preserve path editing and tab focus. Local and SSH viewer tests now close jptxt and assert actual Home/Down/Up/Down cursor movement; the SSH test continues through F5. Native smoke covers right-panel input restoration, unfinished path editing and tabs. Windows build and both core suites pass.

## 2026-09-22: media viewer design

Researched mpv/libmpv playback, render and stream APIs. Chose an optional standalone C++ player as the proposed first host, with images and MP4 in the first usable release, then aicmd Space integration and seekable SFTP sources. Specified exact navigation/seek/frame keys, marking-key conflict resolution, folder snapshot ordering, focus-return contract, IPC/tool commands, budgets and acceptance tests. Design only; runtime selection still needs the backend spike.

## 2026-09-22: aiview, clipboard and agent tools

Implemented native Windows aiview with pinned libmpv development runtime, D3D11 playback, C++ media controller, mixed-folder navigation and the requested keys. Added dedicated SFTP read-at sources and a 64 MiB block cache; remote video no longer needs full-file staging when opened with Space. Integrated frozen commander folder snapshots, independent marks and final-item return. Native integration testing exposed and fixed missing projection of the returned item into the visible list cursor.

Added Ctrl+C/Ctrl+Insert pixel copying, asynchronous operation status, local user-restricted named-pipe controls, JSON schemas and MCP stdio adapter. Explicit agent frame capture returns bounded PNG image content independently of clipboard. Tests caught GPU screenshots including the transparency checkerboard: switched capture to software-accessible decoded pixels, retained alpha, and applied display rotation explicitly. Added CF_DIBV5 and a composited fallback. A large PNG test caught nonblocking pipe buffer limits; responses now write bounded chunks with progress-based timeouts.

Validation: Windows/Linux shared suites (58 + 14 + 22 checks), 28 native media checks including HEVC and variable frame rates, SFTP tail-index seeking and reverse stepping with about 1.8 MB fetched, real clipboard pixels, rotation/transparency and MCP image responses. Commander/native text-viewer regressions are tracked in Status. Runtime/header provenance and remaining packaging/portability/performance gates are documented in Aiview.md and third_party/mpv/README.md. No Gemini account or external model connection was configured.
