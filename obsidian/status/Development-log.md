# Development log

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
