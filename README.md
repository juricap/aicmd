# aicmd

A minimal native, two-panel file manager following the compact, keyboard-first Total Commander / Midnight Commander workflow. C++17 and native APIs, following `../jptxt`.

**v0.1.0 development:** Windows browsing, tabs, SSH/SFTP, F3 viewing, F5 copying, shared core, CLI, and persistence implemented. SSH terminal and model transports are planned on the same core.

- [Vault home](obsidian/Home.md)
- [Design](obsidian/docs/Design.md), [reference study](obsidian/docs/Reference-study.md), [shortcuts](obsidian/docs/Shortcuts.md)
- [Todos](obsidian/todos/M1-Windows.md), [status](obsidian/status/Status.md), [development log](obsidian/status/Development-log.md)

## Build and run

```powershell
.\build.bat
.\build\aicmd.exe
.\build\aicmd.exe F:\devel C:\Users
.\build\aicmd-cli.exe list .
.\build\aicmd-cli.exe commands
```

Requires Visual Studio 2022 C++ tools; `build.bat` configures Release, builds, and runs core tests. CMake also supports other developer environments.

Validation: `scripts\test-gui.ps1`, `scripts\benchmark.ps1`, `scripts\test-linux.ps1`. Windows and Linux pass the core, SSH/copy, media, copy-queue and file-operation suites. Details and limitations are in [status](obsidian/status/Status.md).

Two full-width lists with compact tabs, path bars, and columns. Enter/Backspace browse; Tab switches panel; Ctrl+T/W creates/closes tabs; Ctrl+L edits the path; Insert/Shift+Space marks entries; Space opens media (marks other entries); F3/F4 view/edit in jptxt; F5 copy, F6 move, Shift+F6 rename, F7 new folder, F8 Recycle Bin, Shift+F8 permanent delete. Nothing is ever overwritten. Full list: [shortcuts](obsidian/docs/Shortcuts.md).

Session: `%LOCALAPPDATA%\aicmd\session.json`. Use `--session <file>` for an independent window. Set `AICMD_EDITOR` or `--editor <jptxt.exe>` if jptxt is not found automatically.

Linux checks run via `ssh ai`, in an isolated directory under `/data/devel/`. **Do not use WSL.**

Development: https://github.com/juricap/aicmd, branch main.

Media: optional native **aiview** plays local/SSH images, video and audio. Run `scripts/setup-aiview.ps1` once for the pinned libmpv runtime, then build normally. Space opens media; PgUp/PgDn/Home/End browse; arrows seek five seconds; comma/period step frames; Ctrl+C copies the image/frame. A local MCP/function-call adapter also exposes controls and explicit PNG frame capture. See [aiview usage and agent API](obsidian/docs/Aiview.md).

SSH: Alt+F1/F2 -> SSH -> config alias. F3 views; F5 queues copies to the other panel with byte/entry progress. Escape cancels the displayed transfer; Shift+Escape cancels all. Large selections run sequentially in the background. See [copy jobs and next steps](obsidian/docs/Copy-jobs.md). See [SSH usage and limits](obsidian/docs/SSH.md). The config is rediscovered whenever the drive menu opens.

```powershell
.\build\aicmd-cli.exe ssh-hosts
.\build\aicmd-cli.exe list ssh://ai/data/devel
.\build\aicmd-cli.exe copy local.txt ssh://ai/data/devel/new.txt
```
