# SSH browsing, viewing and copying

Implemented 2026-09-22. Windows GUI and the shared Windows/Linux core use SFTP v3 through the installed OpenSSH client. No remote helper installation or remote shell listing parser is needed.

## Usage

- Alt+F1 / Alt+F2: choose **SSH**, then a configured alias for the left/right panel. Connections start at `/`.
- Ctrl+L: enter `ssh://ai/data/devel` or another alias/path. Within an SSH tab, `/etc` stays on that server. Enter, Backspace, tabs, history, sorting, marks and bookmarks work as for local locations.
- F3 (or Enter on a remote file): download to a managed local stage and open read-only in jptxt. An unchanged staged file is reused after checking server size/mtime and the local stage's size/mtime.
- F5: copy marks, or the focused entry, to the opposite panel. Confirm the destination in the dialog. Supports local/local, upload, download, remote/remote and recursive directories. Escape cancels; the footer shows transferred bytes.
- Ctrl+R retries a disconnected/failed listing. Existing destinations are refused; merge/overwrite/resume policies remain future work.

The application discovers concrete `Host` aliases in `%USERPROFILE%\.ssh\config` (or `~/.ssh/config` on Linux) at startup and whenever the drive menu opens. It handles comments, multiple aliases, quoted aliases and bounded/cycle-checked Include files, including filename globs. Wildcard/negated host patterns do not become selectable machines. Alias discovery is not an SSH configuration evaluator: OpenSSH applies Host/Match rules, IdentityFile, User, HostName, ProxyJump, etc. Conditional Includes can therefore expose aliases that are not connectable under the current conditions. Directory-component globs and token-expanded Include paths are not yet discovered.

Authentication is noninteractive: configured keys/agent and already-trusted host keys. Unknown/changed host keys and password prompts fail visibly; use ordinary `ssh alias` to establish trust or unlock credentials first. Forwardings are disabled for the file-manager connection. No passwords or private keys are stored by aicmd.

## Shared implementation

`Location` distinguishes local native paths from `{host, remote POSIX path}`. Remote paths never flow into local enumeration APIs. Session JSON stores remote authority/path separately and continues reading old local-only schema-1 sessions. The default provider routes listings; the core queues view/copy jobs and exposes progress/cancellation via `file_view`, `file_copy`, `job_get`, `job_cancel`.

The built-in SFTP client uses a persistent SSH process per alias and lane (browse/read/write), up to 48 lanes. File bytes remain binary, with 16 outstanding 32 KiB requests per batch (512 KiB payload window), so the client does not wait one network round trip per block. Cross-server and same-server copies stream through the client with bounded buffers; server-side copy extensions are not implemented. Listings collect metadata directly from SFTP; only symlinks require follow-up stat calls.

Workers own all connection I/O. Cancellation or 30 seconds without pipe progress terminates that lane's SSH process tree; the next operation reconnects. Another lane can keep browsing while copying. No failed write is automatically replayed. Windows uses restricted inherited pipe handles and a child Job Object; POSIX uses a dedicated process group and close-on-exec descriptors.

## Copy and cache semantics

Files stream into exclusively-created `.aicmd-part-*` siblings, then publish with no-replace rename/link behavior. Local files are flushed before publication. Remote server durability is not guaranteed by SFTP CLOSE. Source size/mtime changes during copying fail the operation; same-size edits within the server timestamp resolution are not reliably detectable. Files created before a later failure remain, as do newly-created directories. A connection failure can leave a remote staging file to clean up; this is reported in the GUI.

Recursive copying refuses symlinks and special files, limits depth to 64 and total visited entries to one million, and rejects destinations inside the source (using canonical paths where available). It does not preserve timestamps/permissions/ownership; remote files start with mode 0600 and directories 0700. Windows-incompatible remote filenames fail copying rather than being silently renamed. UTF-8 remote names are supported; arbitrary non-UTF-8 server filenames on Windows remain a limitation.

Remote F3/jptxt viewing stages at most 1 GiB per file, within a 2 GiB disk-cache admission budget under `%TEMP%\aicmd-view`. Cache checks use metadata, not cryptographic content versions. Staged files remain on disk so an open external viewer is never invalidated; when full, close old views and clean that folder. F5 has no 1 GiB limit. Automatic staging-cache lifecycle cleanup remains planned.

**Space on media now opens [aiview](Aiview.md)** with seekable SFTP byte reads and a bounded block cache. Images/video/audio do not use the jptxt staging cache or its 1 GiB limit. Playback, seeking, frame stepping, clipboard and agent capture use the same decoded media path as local files.

**Editing a staged view is local only.** F4 inside jptxt can enable local editing, but saving does not upload. aicmd F4 on a remote path explicitly reports that remote edit/upload-on-save is unavailable. Conflict-aware remote editing remains a separate feature.

## Validation

- Windows and Linux: 58 existing core checks plus 14 offline SSH/copy checks.
- Windows to `ai`: 45 checks including 8 MiB binary roundtrip, Japanese/spaced filename, recursive directories, remote-to-remote copy, empty files, no-overwrite, cancellation/reconnect, cache reuse/edit detection, remote session persistence and core view/copy jobs.
- Native Windows GUI: remote browsing, actual F3 jptxt handoff and confirmed F5 download; screenshot inspected. Standard native smoke also verifies 100 cursor movements do not invoke layout again.
- Measured 8 MiB upload + download: about 1.8 seconds on this LAN including first connection startup. Not a WAN or maximum-throughput benchmark.
- Linux build/offline tests verified over `ssh ai` in `/data/devel/aicmd-check-20260922`; the Linux SFTP transport and macOS GUI were not live-tested.

Reproduce: `build.bat`, `scripts/test-gui.ps1`, `scripts/test-ssh-gui.ps1`, `scripts/test-linux.ps1`. Run `build/aicmd-ssh-tests.exe ai /data/devel/aicmd-ssh-check-<unique>` after creating that empty isolated directory. Test data is intentionally retained remotely.

Protocol references: [OpenSSH specifications](https://www.openssh.org/specs.html), [SFTP v3 draft](https://www.ietf.org/archive/id/draft-ietf-secsh-filexfer-02.txt), [SSH config](https://man.openbsd.org/ssh_config). Implementation is original C++; these references are protocol documentation.
