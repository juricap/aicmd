# Status

2026-09-21 — v0.1.0 development. First usable compact Windows browser implemented and validated. Broader M1 acceptance items remain in the todo list.

[Home](../Home.md), [todos](../todos/M1-Windows.md), [design](../docs/Design.md).

## Works

Shared C++17 core, Windows native GUI, console listing/benchmark host, asynchronous enumeration, independent panel tabs, navigation history, sorting/filtering, selection, jptxt editing launch, and session persistence.

Current UI follows the user's compact Total Commander / mc setup: full-width file lists, small tabs/path bars, no sidebar or toolbar buttons.

## Verified

- Windows x64 MSVC 19.33 release build; **54 core checks pass**. Unicode/Japanese/emoji names, long paths, cancelled/stale scans, link identification/cycle prevention in the experimental core, history, independent tabs, failed paths, sorting, resource limits, and session corruption/round-trip are covered.
- Native GUI smoke passes with real controls: full-width/compact geometry, tabs, parent/close, keyboard-routing for panel/tab/path/marks, and valid session output. Screenshot inspected at 100% DPI. This is not a screen-reader or all-DPI certification.
- Linux core/CLI **build without warnings and pass the same 54 checks**, via `ssh ai` on `jurzai1`, GCC 11.4 / CMake 3.22.1. Isolated checkout: `/data/devel/aicmd-check-20260921`. Actual CLI directory listing verified. No Linux GUI/TUI exists yet.

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

No file mutation, TUI, production model transport, macOS/Linux GUI, or image viewer plugins yet. Listings publish only when enumeration and sorting complete. Network cancellation is cooperative; blocked OS I/O can delay shutdown. Per-directory budget is 32 MiB; retained listing budget is approximately 128 MiB. These are estimates of listing storage, not process RSS guarantees.

## Repository

GitHub: https://github.com/juricap/aicmd, branch main. Source, tests, vendored JSON/license, design reference screenshot, and vault are tracked. Build products and test artifacts are ignored.

## Viewer and selection update

F3 now uses jptxt protocol 2 in enforced read-only mode; F4 in jptxt enables text editing in place. Ctrl+A and keypad select/unselect/invert route through the shared core. Marks use red text with an independent name-cell cursor. Native smoke covers wildcard masks and focus return from the dialog. Image and remote filesystem plugins are design work only; see [plan](../docs/Plugins-and-remote-filesystems.md). The binary measurements above describe the initial browsing baseline, before this update.

Actual Windows F3 handoff passed with a spaced filename, using the sibling jptxt binary. Its separate viewer suite passes 29 checks on Windows/Linux and native GUI tests for read-only enforcement, same-tab editing, and single-instance delivery. Cocoa changes remain unverified.
