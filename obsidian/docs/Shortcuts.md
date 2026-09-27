# Keyboard shortcuts

| Key | Action |
|---|---|
| Tab | Switch panel |
| Up / Down / Home / End / Page Up / Page Down | Move focused row, retaining marks |
| Enter | Enter directory or edit focused file in jptxt |
| Backspace | Parent directory |
| Alt+Left / Alt+Right | Back / forward |
| Insert | Toggle mark and advance |
| Space | Open recognized media in aiview; toggle mark on other entries |
| Shift+Space | Toggle mark, including media |
| Shift+Up / Down | Mark the row being left |
| Ctrl+A | Mark all files and directories, excluding parent row |
| Ctrl+L | Edit location in place; Enter applies, Escape cancels |
| Ctrl+T / Ctrl+W | New / close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+F6 | Cycle list, path editor, tabs |
| Ctrl+R | Refresh / retry |
| Ctrl+H | Show hidden/system entries |
| F4 | Open focused file in jptxt |
| Escape | Cancel navigation (or cancel path edit) |
| Alt+F4 | Exit and save session |

F3 opens jptxt read-only; F4 inside jptxt enables editing without restarting. F5 copies to the other panel; F6-F8 remain reserved.

| Selection key | Action |
|---|---|
| Num+ / Num- | Select / unselect wildcard masks; semicolon separates masks |
| Shift+Num+ / Shift+Num- | Select / unselect everything |
| Num* | Invert all marks |

Marks use red text; the full-row highlighted cursor moves independently. Masks support *, ?, and *.* including extensionless names.

Mouse: a plain left click anywhere across a file row moves only the cursor and preserves existing marks. Ctrl+click toggles that row's mark; Shift+click adds the range from the current cursor. The parent row cannot be marked. Double-click opens the clicked entry; clicking empty space preserves cursor and marks. Keyboard and mouse navigation can be freely alternated.

| Navigation key | Action |
|---|---|
| Ctrl+D | Folder bookmarks: Add current folder, remove current folder, or jump to a saved folder |
| Alt+F1 / Alt+F2 | Choose a Windows drive for the left / right panel |

Drive letters and bookmark numbers are menu accelerators; arrows/Enter select and Escape cancels. Bookmarks persist with the session and are shared by both panels.

SSH: Alt+F1/F2 -> SSH -> alias. Ctrl+L accepts ssh://alias/path. F3 downloads a cached read-only view; Enter on a remote file also views. F5 copies marks or focus to the opposite panel after confirmation; Escape cancels a transfer. Remote editing/upload-on-save is not available yet.

Media viewer: PgUp/PgDn/Home/End browse folder media; Left/Right seek 5 seconds; comma/period step frames; Space play/pause; Ctrl+C or Ctrl+Insert copies the decoded image/frame; Escape closes. F toggles fullscreen; +/-/0/1 zoom/fit/actual pixels for stills. See [aiview](Aiview.md).
