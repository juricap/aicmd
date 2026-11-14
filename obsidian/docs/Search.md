# Search (Alt+F7)

[Home](../Home.md) · [shortcuts](Shortcuts.md) · [status](../status/Status.md)

Alt+F7 (or File → Search) asks for a start folder (local or `ssh://alias/path`), a wildcard mask (`*.cpp;*.h`) and optional text. Results stream into a new tab in the active panel, in discovery order, as paths relative to the start folder. In that tab:

- Enter on a file **goes to the file**: opens its folder with the cursor on it (Total Commander "Go to file"). Enter on a folder enters it.
- `[..]` returns to the searched folder. Ctrl+R reruns the search. Escape stops it and keeps the results.
- F3/F5/F6/F8/marks work on results as on any listing. Sorting by a column header sorts the results.
- Hidden/system entries follow the panel's Ctrl+H setting. Links and junctions are listed but never followed.

CLI: `aicmd-cli search <folder> <mask> [text]` prints relative paths, and statistics to stderr.

## Scheduling

The goal is to reach as much of the tree as quickly as possible, and never get stuck behind one large folder.

```
           ┌──────────── fast queue (by depth, FIFO) ◄──────────────┐
start ───► │ fast lane 1 ─┐  capped listing: ≤4096 entries, ≤150 ms │ subfolders
           │ fast lane 2 ─┤── fits?  yes → match names, queue subfolders
           └──────────────┘          no  → hand folder to slow lane ─┐
                                                                     ▼
                                        slow lane: streams the whole folder,
                                        publishes matches and queues subfolders as it goes
name matches with a text condition ──► content lane (streams file, 1 MiB chunks)
```

- **Start folder first, completely.** The start folder is the only queued item; its subfolders are queued, never entered, so every entry of the start folder is examined before any child (verified by test).
- **Breadth-first.** Fast lanes always take the shallowest queued folder, so shallow results appear first and a deep or huge branch cannot monopolise the search.
- **Probe instead of a separate counter.** Each fast lane's listing is itself the probe: it lists at most 4096 entries or 150 ms. A folder that fits is processed at once; a folder that does not is handed (unprocessed, so nothing is reported twice) to the single slow lane. The wasted work is bounded by the cap. This gives the "check depth and file counts first" behaviour without listing each folder twice.
- **Slow lane** streams large folders in full, flushing matches every 2048 entries or 100 ms, and feeds subfolders back to the fast lanes while it continues.
- **Content lane** checks text matches separately so slow reads never stall traversal; listers wait only if more than 100k candidates are pending.
- **Bounds:** 100k results (then stops, marked truncated), depth 256, and if more than 200k folders are queued the fast lanes drain deepest-first to limit memory.
- **SSH:** each lane uses its own pooled SFTP connection (`search-1`, `search-2`, `search-slow`, `search-content`), separate from browsing and copying.

## Measurements (Windows, i7-8700, warm cache, `aicmd-cli search`)

| Search | Results | Folders | Entries | Slow-lane folders | First hit | Total |
|---|---|---|---|---|---|---|
| `F:\devel` `*.cpp` | 529 | 92,614 | 1,019,274 | 7 | 23 ms | 15.7 s |
| `C:\Windows` `notepad.exe` | 13 | 185,032 | 396,720 | 9 | 16 ms | 37.9 s |
| `C:\Windows\System32` `*.dll` | 5,630 | 1,997 | 28,989 | 2 | 12 ms | 0.23 s |

PowerShell `Get-ChildItem C:\Windows -Recurse -Filter notepad.exe` took 65 s on the same machine (not a tuned baseline). Lane count and caps are untuned defaults; cold-cache, SMB and WAN behaviour are not yet measured. Remote search over `ssh://ai` was verified functionally (names and content), not benchmarked.
