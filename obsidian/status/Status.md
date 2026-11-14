# Status

2026-11-14 release-packaging increment (v0.2.0): VERSIONINFO for every executable from the CMake project version; Help menu with F1 shortcuts and About; jptxt is now optional (F3 opens the associated application, F4 the registered editor or Notepad, with a notice); Space falls back to marking when aiview.exe or libmpv-2.dll is absent. `scripts/package.ps1` builds, runs tests, stages aicmd/aicmd-cli/aiview/aiview-tool with README.txt and third-party notices, verifies each binary's version, runs the full GUI smoke **from the packaged copy**, runs the packaged CLI, and writes `artifacts/release/aicmd-<version>-win-x64.zip` plus SHA-256. libmpv (`-IncludeMpv`) and jptxt (`-IncludeJptxt`) are opt-in because their licensing is separate. Verified: Windows suites, GUI smoke, packaged smoke, Linux suites. Not verified: the no-jptxt and no-libmpv fallbacks interactively, installers/code signing. **Blocking public release: the repository has no LICENSE yet** (package script warns).

2026-11-14 file-operations increment: F6 move, Shift+F6 rename, F7 new folder, F8/Delete Recycle Bin and Shift+F8/Shift+Delete permanent delete, locally and over SSH. Every operation is a core job (`file_move`, `file_rename`, `file_delete`, `dir_create`) with no-overwrite semantics: Windows `MoveFileExW` without replace, Linux `renameat2(RENAME_NOREPLACE)`, macOS `renamex_np(RENAME_EXCL)` (untested), OpenSSH SFTP rename. Moves rename when possible; across volumes/hosts they copy, then delete the source only after the full copy succeeds. Rename/mkdir run on the general workers so they never wait behind long transfers; move/delete share the transfer FIFO, progress line and Escape cancellation. Affected visible panels refresh automatically and focus the new/renamed entry.

Verified: Windows build (/W4, no warnings) and 5 CTest suites including new **25 file-operation checks** (Recycle Bin, directory symlink not followed, read-only files, no-overwrite rename/move, move-into-self, cancellation, core refresh/focus, stale snapshots); Linux isolated build `/data/devel/aicmd-check-fileops-20261114` passes all 5 suites without warnings; live Windows-to-ai SSH suite **56 checks** including remote mkdir, Unicode rename, no-overwrite, local↔remote moves and recursive remote delete. Native GUI smoke now drives F7, Shift+F6, F6 and Shift+Delete through real key routing (15 stages); copy, viewer and SSH GUI regressions pass. Rename dialog inspected visually. Not verified: Recycle Bin on non-NTFS fixed drives, deleting very large trees for time, macOS rename path, conflict prompts (conflicts still fail the job).

2026-10-01 copy increment: F5 accepts successive local/SSH copy batches into a bounded FIFO serviced by a dedicated worker. Progress reports bytes, selected-root counts and queue depth; Escape cancels the displayed transfer and Shift+Escape cancels all window transfers. Browsing remains independent. Existing destinations fail that job without prompting; later jobs continue. Completed files remain on interruption. Jobs currently end with the process. See [copy design](../docs/Copy-jobs.md) for deferred decisions, durable journals, agent commands and unattended notifications.

Verified this increment: all four Windows and Linux CTest suites; live Windows-to-ai SSH suite (49 checks, including queued 8 MiB uploads and cancellation); native browsing/viewing/copy regression; native 2,000-file progress, second queued batch, modifier-controlled Shift+Escape cancellation and post-copy navigation. Linux isolated build: `/data/devel/aicmd-check-copy-20261001`. GUI artifacts are ignored under `artifacts/copy-gui-*`. These checks do not certify WAN performance, crash recovery or hours-long unattended operation.

2026-09-27 mouse follow-up: fixed native left-click selection conflicting with independent keyboard cursor/marks. Full-width row clicks now move the cursor without turning the clicked item red or clearing existing marks. Ctrl+click toggles marks; Shift+click adds a range. Native smoke verifies alternating keys/clicks across name, metadata and row-tail positions, panel focus, blank space, parent-row exclusion and double-click navigation. Windows build (all three test suites), native GUI smoke and media/commander return tests pass. This is a Windows UI change; Linux code is unchanged.

2026-09-22 — v0.1.0 development. First usable compact Windows browser implemented and validated. Broader M1 acceptance items remain in the todo list.

[Home](../Home.md), [todos](../todos/M1-Windows.md), [design](../docs/Design.md).

## Works

Shared C++17 core, Windows native GUI, console listing/benchmark host, asynchronous enumeration, independent panel tabs, navigation history, sorting/filtering, selection, jptxt editing launch, and session persistence.

Current UI follows the user's compact Total Commander / mc setup: full-width file lists, small tabs/path bars, no sidebar or toolbar buttons.

## Verified

- Windows x64 MSVC 19.33 release build; **58 core checks pass**. Unicode/Japanese/emoji names, long paths, cancelled/stale scans, link identification/cycle prevention in the experimental core, history, independent tabs, failed paths, sorting, resource limits, and session corruption/round-trip are covered.
- Native GUI smoke passes with real controls: full-width/compact geometry, tabs, parent/close, keyboard-routing for panel/tab/path/marks, and valid session output. Screenshot inspected at 100% DPI. This is not a screen-reader or all-DPI certification.
- Linux core/CLI **build without warnings and pass the same 58 checks**, via `ssh ai` on `jurzai1`, GCC 11.4 / CMake 3.22.1. Isolated checkout: `/data/devel/aicmd-check-20260921`. Actual CLI directory listing verified. No Linux GUI/TUI exists yet.

## Measurements

Windows machine: Intel Core i7-8700, MSVC Release x64, local `F:` fixtures, zero-byte files, warm filesystem cache. Each fixture measured for 20 runs. Includes worker enumeration, final sort, snapshot commit, and CLI polling; does not measure window startup, first paint, or input latency. The first run also waits for the second panel; subsequent runs refresh the left panel.

| Item | Result |
|---|---|
| GUI `aicmd.exe` | 566,272 bytes (553 KiB), static CRT |
| CLI `aicmd-cli.exe` | 374,272 bytes (366 KiB) |
| 10,000 entries | median 31.17 ms, p95 32.14 ms |
| 100,000 entries | median 218.90 ms, p95 233.74 ms |

Raw benchmark logs, native smoke reports, screenshots, and disposable fixture/session data live in ignored `artifacts/`.

Reproduce: `build.bat`, `scripts/test-gui.ps1`, `scripts/benchmark.ps1`, `scripts/test-linux.ps1`.

## Limits

No overwrite/merge prompts, TUI, commander model transport, macOS/Linux GUI, or public image viewer plugins yet. Aiview now has its own local media MCP/tool transport. Listings publish only when enumeration and sorting complete. Network cancellation is cooperative; blocked OS I/O can delay shutdown. Per-directory budget is 32 MiB; retained listing budget is approximately 128 MiB. These are estimates of listing storage, not process RSS guarantees.

## Repository

GitHub: https://github.com/juricap/aicmd, branch main. Source, tests, vendored JSON/license, design reference screenshot, and vault are tracked. Build products and test artifacts are ignored.

## Viewer and selection update

F3 now uses jptxt protocol 2 in enforced read-only mode; F4 in jptxt enables text editing in place. Ctrl+A and keypad select/unselect/invert route through the shared core. Marks use red text with an independent full-row highlighted cursor. Native smoke covers wildcard masks and focus return from the dialog. Image plugins and additional remote protocols remain design work; SSH/SFTP is now built in; see [plan](../docs/Plugins-and-remote-filesystems.md). The binary measurements above describe the initial browsing baseline, before this update.

Actual Windows F3 handoff passed with a spaced filename, using the sibling jptxt binary. Its separate viewer suite passes 29 checks on Windows/Linux and native GUI tests for read-only enforcement, same-tab editing, and single-instance delivery. Cocoa changes remain unverified.

## Full-row cursor, bookmarks and drives

Restored the full-row dark cursor highlight to match the reference, with red text retained for marks. Ctrl+D opens the saved-folder menu; Alt+F1/F2 opens current Windows drive letters for the left/right panel. Bookmark add/remove commands belong to the shared core and persist as an optional schema-1 field, preserving compatibility with old sessions. Native GUI smoke exercises bookmark creation and drive selection through the real popup menus. Windows/Linux core tests pass 58 checks; the full-row cursor screenshot was inspected.

## SSH and redraw update (2026-09-22)

Removed layout, unchanged label/header updates and whole-list invalidation from cursor movement. Native smoke verifies 100 arrow movements invoke no layout passes. Full-row cursor and independent red marks remain.

Added real SSH config discovery to drive menus, typed remote locations, persistent SFTP browsing and pipelined view/copy jobs. Windows and Linux pass 58 core + 14 offline SSH/copy checks. Live Windows-to-ai testing passes 45 checks; actual GUI remote F3 and F5 also pass. The 8 MiB upload/download roundtrip was byte-exact and took about 1.8 seconds including connection startup. Current Linux checkout: /data/devel/aicmd-check-20260922. See [SSH](../docs/SSH.md) for authentication, overwrite, staging and remote-edit limitations.

Current release build: aicmd.exe 755,200 bytes; CLI 526,848 bytes. The table above remains the initial baseline.

Viewer-return keyboard fix: the active window now restores its panel input control. Verified actual navigation after closing local and SSH jptxt views, plus path-editor/tab preservation. Updated scripts/test-viewer.ps1 and scripts/test-ssh-gui.ps1 use scripts/assert-navigation.ps1 to check row movement, not merely foreground state.

## aiview implementation (2026-09-22)

Implemented separate native Windows `aiview.exe`, optional pinned libmpv runtime, portable media controller and seekable local/SFTP sources. Space launches media in commander's frozen displayed order; Insert/Shift+Space retain marking. Folder keys, five-second seeks, forward/backward frame stepping, play/pause, zoom, fullscreen and volume controls are implemented. Viewer exit returns the browsed item without losing marks or keyboard navigation.

Ctrl+C/Ctrl+Insert copy full-resolution decoded pixels to Windows clipboard (alpha-capable CF_DIBV5 plus black-composited CF_DIB fallback). Model adapters share typed commands. The user-restricted local pipe and `aiview-tool --mcp` expose playback tools and explicit PNG image capture without touching clipboard or automatically contacting a model. Gemini integration itself remains untested.

Verified: Windows build and 58 core + 14 SSH/copy + 22 media-controller/read-at checks; Linux same suites through `ssh ai` at `/data/devel/aicmd-check-media-20260922`. Native media suite passes **28 checks**, including JPEG/PNG, H.264/AAC with D3D11 decode, HEVC, WAV seek state, CFR/VFR stepping against FFprobe timestamps, EXIF rotation, transparent-image copying, MCP image content and large PNG pipe responses. Remote tail-index MP4 playback/seek/reverse step fetched about 1.8 MB from a roughly 130 MB sparse fixture. Captured frame/PNG pixels were inspected. This is functional LAN validation, not an internet-streaming benchmark.

Standalone and commander Space/mark/current-item/focus tests pass. Existing native GUI, local F3/jptxt and SSH F3/F5/keyboard-return regression scripts also pass. Portable GUI shells, binary redistribution/license manifest, 4K/HDR fidelity and long-run/WAN interruption hardening remain open. Native Windows rendering uses libmpv's `wid`/D3D11 adapter; initial commander attachment uses bounded temporary snapshots and one viewer at a time. Details, reproducible commands, limits and tool contracts: [aiview](../docs/Aiview.md).
