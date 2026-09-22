# Plugins, viewer formats, and remote filesystems

2026-09-21 — design contract and delivery plan; plugin runtime and additional remote providers remain planned. A built-in SSH/SFTP provider was implemented on 2026-09-22; see [current SSH implementation](SSH.md). User explicitly requested image viewing and remote browsing via SSH/FTP/CIFS. [Main design](Design.md), [todos](../todos/M1-Windows.md).

## Two distinct extension points

1. **jptxt viewer plugins** detect and render content (text, hex, images, later other formats).
2. **aicmd filesystem providers** expose locations, metadata, listing, reads, and optionally mutations (local, SFTP, FTP/FTPS, SMB, archives later).

A viewer receives a read-only stream or local staged file; it does not need SSH credentials or network protocol knowledge. A provider does not draw file panels or implement model tools. GUI, TUI, and model dispatch use the same provider commands and capabilities.

## Viewer host and image plugin

The current jptxt 0.2.0 text viewer supports `--readonly -- <path>` and F4 enables text editing in the same tab without reopening the file. Binary/hex files remain noneditable. Images currently fall back to hex; image rendering is the next separate viewer capability.

Introduce a `ViewerDocument` abstraction alongside the existing text `Doc`, with immutable source identity, content metadata, view state, and an explicit editable/read-only mode. Detection order: bounded content sniffing, then filename hint; filename extensions alone do not select a decoder. Text and hex become built-in implementations of the same host contract.

Initial image plugin: PNG, JPEG, BMP, and static GIF; fit-to-window, 100% size, zoom, pan, rotate-view, checkerboard transparency, image dimensions, and orientation metadata. Rotation initially affects the view only. Animated images, SVG, RAW, and image editing are later additions. F4 on an image reports that the selected renderer has no editing capability; it must never send binary bytes to the text editor as an editable document.

Candidate Windows decoder: [Windows Imaging Component](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-about-windows-imaging-codec). Decode off the UI thread into bounded CPU image buffers, then paint through the native backend. Cross-platform decoder choice remains open pending footprint and format tests; renderer behavior must not depend on WIC-specific types.

Version-1 viewer operations:

| Operation | Contract |
|---|---|
| `probe` | Read at most 64 KiB; return confidence, media type, renderer ID, and capabilities |
| `open` | Receive read-only source token; return document token and metadata |
| `render` | Receive viewport, zoom, page/frame, and request generation; produce a bounded RGBA tile/image |
| `set_mode` | Explicit transition to editing only if the renderer advertises `edit`; retain document/view state |
| `close` / `cancel` | Release source/buffers or cancel generation; late frames are discarded |

Start with a built-in image adapter to validate this interface, then support separately installed helper processes. The public ABI must not expose C++ STL, `Doc*`, HWND, Cocoa, or X11 objects. For a future in-process C ABI, require `abi_version`, `struct_size`, opaque handles, explicit allocator ownership, and a query-capabilities function. Do not freeze that ABI before the image adapter proves the host interface.

External decoders run as helpers by default: parser crashes become a plugin error with a text/hex fallback. Limit source read size, decoded pixel count (initial ceiling 100 MP / 400 MiB RGBA, configurable lower), output bytes, outstanding requests, and decoder time. Never auto-load a plugin from the directory being viewed. Installed plugins are explicitly trusted code; a helper process alone is not an OS sandbox, so filesystem/network restrictions require platform sandbox work before claiming isolation.

## Provider identity and path contract

Replace local-only `fs::path` at the application boundary with `Location { provider_id, connection_id, opaque_path_token, display_path }`. Local paths retain their current native representation behind the local provider. POSIX names, Windows UTF-16 names, SFTP names, and server-defined encodings must round-trip without deriving authoritative paths from display strings.

Persist connection IDs and location tokens, not passwords, private keys, or access tokens. Restore a disconnected tab with its label/location and a reconnect action. Tabs from different providers coexist in either panel.

Provider v1:

| Operation | Required behavior |
|---|---|
| `connect`, `disconnect`, `capabilities` | Explicit authority, authentication through host credential broker, capability discovery |
| `list` | Paged immediate children, opaque cursor, optional attributes, cancellation, partial/error state |
| `stat`, `read_range` | Metadata and bounded random/sequential reads; source version token where available |
| `resolve`, `parent` | Provider-native canonical resolution, parent/root checks, symlink behavior |
| `write`, `mkdir`, `rename`, `remove` | Optional; advertise atomicity, overwrite rules, and trash support separately |
| `watch` | Optional; refresh fallback always works |

Third-party providers are helper executables speaking versioned JSON-RPC over private stdio, with request IDs, bounded metadata messages, and a framed binary data channel for file bytes. Native C/C++ or Rust helpers are both supported. Built-in providers can implement the typed interface directly. No plugin owns or mutates the panel state. All calls pass through shared jobs, cancellation, error handling, and model capability/root policy.

Connection state: disconnected -> connecting -> ready -> degraded/error -> reconnecting. Reads may retry with a visible status; writes must not replay automatically after an uncertain disconnect. Cancellation closes the request/channel when the library supports it and discards late results. Cache listings per connection/version; remote I/O never blocks the UI/session executor.

## Protocol plan

| Provider | Plan | Notes |
|---|---|---|
| Local / mounted volumes | Existing local provider | Windows UNC shares and OS-mounted SMB/SSHFS paths use OS authentication; GUI remote-share behavior still needs live validation |
| SFTP over SSH | First direct remote provider | Candidate [libssh2](https://github.com/libssh2/libssh2) supports SSH2/SFTP. Use agent or explicit key identity, host-key verification, password only through credential broker, per-connection root and host identity |
| SSH command bridge | Alternative transport | `ssh host aicmd-cli --rpc` later; useful where aicmd can run remotely. Separate from the terminal frontend over an interactive SSH PTY |
| FTP / FTPS | Optional provider helper | Candidate [libcurl](https://curl.se/docs/features.html); prefer FTPS profile, make plain FTP an explicit connection choice. Directory listing parsing and server encodings must be tested |
| SMB2/SMB3 (CIFS) | OS paths first; direct plugin later if needed | Windows UNC / mounted POSIX paths avoid shipping an SMB stack. A direct helper must negotiate modern SMB and integrate credentials; do not introduce SMB1 as the default |

These are candidate libraries, not newly vendored dependencies or promises of protocol support. Pin versions and evaluate licenses, dependency size, cancellation, and platform builds before implementation.

## Remote viewing and editing

F3 passes a provider-backed read-only stream to the viewer host; when a renderer needs a local path, stage a bounded file in a private cache and retain origin/version metadata. Never expose credentials to the renderer. Closing the viewer releases the stage when no longer needed.

F4 / Enable editing on a staged text file changes mode in the same viewer tab. Saving becomes a host-controlled upload: compare the remote version/identity against the download, prompt on conflicts, upload to a temporary sibling and rename only if the provider supports that safely. Otherwise report the weaker semantics before applying. Disconnection leaves a recoverable local edited copy; it must not claim remote save success. Large files without random access can offer sequential viewing or explicit full staging.

Cross-provider copy streams through the core job engine with bounded buffers, progress, cancellation, and partial completion. Remote move between providers is copy/verify/delete, never assumed atomic. Model callers use exactly these contracts and configured authority scopes.

## Delivery order and acceptance

1. Land text view-only / in-place edit switching and reliable mode-aware handoff (current work).
2. Finish commander marking/selection semantics and tests.
3. Refactor `Location`/provider capability boundary; add a fake remote provider covering latency, pagination, disconnection, and stale versions.
4. Add image `ViewerDocument` adapter; verify image fit/zoom, corrupt input, huge dimensions, cancellation, and binary-edit rejection.
5. Add SFTP browsing/read-only viewing against a controlled SSH fixture; verify host-key changes, denied roots, filenames, cancellation, reconnect, and multi-tab independence.
6. Add safe remote staged editing and copy jobs; then optional FTP/FTPS/direct SMB helpers.

TUI testing remains on `ssh ai`, under `/data/devel/`. Do not use WSL.
