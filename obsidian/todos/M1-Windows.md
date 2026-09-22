# M1: compact Windows directory browsing

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
- [ ] M5: copy/move/rename/mkdir/trash with explicit conflicts and partial completion reporting.
- [x] Explicit read-only jptxt viewer contract for F3; F4 enables editing in place.
- [ ] M6: Gemini function/Live adapter over the shared commands.
- [x] Configure GitHub remote and publish main with id_ghjuricap.

- [ ] jptxt image renderer and versioned viewer plugin API.
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
