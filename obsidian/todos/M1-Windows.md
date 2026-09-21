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
- [ ] M5: explicit read-only jptxt viewer contract for F3.
- [ ] M6: Gemini function/Live adapter over the shared commands.
- [ ] Configure GitHub remote after user provides it; no remote configured yet.
