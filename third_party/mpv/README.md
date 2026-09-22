# libmpv development dependency

Headers: unmodified `client.h` and `stream_cb.h` from shinchiro's
`mpv-dev-x86_64-20260921-git-e76a35ec95.7z`, mpv commit `e76a35ec95`.
Their embedded license notices apply. Upstream Copyright is included.

Runtime source: https://github.com/shinchiro/mpv-winbuild-cmake/releases/tag/20260921

Archive SHA256: `361392D64A231830AC79D1C0F91DDB68C6A709ACCD282B2C87B13889D382FDEC`.
Runtime DLL is 120,768,000 bytes; it is intentionally not tracked or bundled.
`scripts/setup-aiview.ps1` installs this pinned x86_64 build into ignored `build/`.
Alternatively set `AIVIEW_MPV_DLL` to an absolute DLL path. We load only that
path or the DLL beside aiview, with dependency lookup restricted to its
directory and Windows system libraries.

This is a third-party development build, not a declaration that it is LGPL.
mpv/FFmpeg license obligations depend on enabled components. Before publishing
a binary bundle, audit the exact build configuration, provide applicable
licenses/notices and corresponding sources as required. The setup download is
not a completed redistributable packaging solution.

Native Windows `wid` + GPU/D3D11 is the first verified rendering adapter.
Portable controller and read-at source types do not depend on this adapter.
