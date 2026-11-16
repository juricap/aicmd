# Archives as folders

[Home](../Home.md) · [shortcuts](Shortcuts.md) · [status](../status/Status.md)

## What Total Commander does (reference)

- Enter or Ctrl+PgDn on an archive opens it like a directory; `..` at the archive root returns to the containing folder. Ctrl+PgUp goes to the parent.
- Built-in handling covers ZIP (read/write) and read-only TAR, GZ/TGZ, CAB, ARJ, LZH, RAR and ACE; other formats come from **WCX packer plugins**, 64-bit DLLs exporting `OpenArchive`, `ReadHeader`, `ProcessFile` and `CloseArchive` (the read-only minimum). See the [WCX SDK overview](https://ghisler.github.io/WCX-SDK/overview.htm).
- F3 in an archive extracts to a temporary file and views it; F5 out of an archive unpacks to the other panel; F5 into an archive, or Alt+F5, packs; Alt+F9 unpacks a whole archive.

## What aicmd does now (read-only)

- **Formats:** ZIP family (`.zip .jar .war .ear .apk .whl .nupkg`) with stored and deflate members, ZIP64, UTF-8 and CP437 names, extended timestamps; TAR (ustar, GNU long names, pax `path`/`size`); `.tar.gz`/`.tgz` including concatenated gzip members. Format is detected from content (gzip magic) and extension.
- **Through 7-Zip's `7z.dll`** (loaded at runtime, Windows): `.7z .rar` (RAR4 and RAR5) `.cab .iso .arj .lzh .lha .wim`, plus `.tar.xz/.txz` and `.tar.bz2/.tbz2/.tbz` (7z.dll decompresses the stream, aicmd's TAR walker indexes it). Release packages bundle the unmodified 7z.dll from 7-Zip 26.06 (`scripts/setup-7zip.ps1`, SHA-256 pinned; LGPL + unRAR restriction, which only forbids building a RAR *compressor*). Lookup order: `7z.dll` beside `aicmd.exe`, then an installed 7-Zip (`HKLM/HKCU\SOFTWARE-Zip` Path64/Path, `%ProgramFiles%-Zip`). Without one, opening such an archive reports how to get it. Encrypted members and header-encrypted archives report that passwords are unsupported. Each member extraction reopens the archive; for solid 7z/RAR that means decompressing up to the member, so copying many files out of a large solid archive is slower than one bulk extraction. Linux: not yet (error message only).
- **Paths:** an archive member is an ordinary location, `F:\x\a.zip\docs\guide.md` or `ssh://ai/data/a.tgz/dir`. The first path component with an archive extension that is a regular file is the container; a folder named `x.zip` stays a folder. Paths typed with Ctrl+L and saved sessions work unchanged.
- **Keys:** Enter / Ctrl+PgDn opens (ZIP, 7Z, RAR, TAR...), Backspace / Ctrl+PgUp / `[..]` leaves with the cursor on the archive. F3 and F4 stage a read-only copy (≤1 GiB). F5 extracts marked files/folders to the other panel (no overwrite). Move, rename, delete, new folder and copying *into* an archive are refused with "archives are read-only". Space inside an archive marks (no media playback from archives yet). The path bar shows `[archive, read-only]`.
- **Remote archives** are read with SFTP random access: a ZIP's central directory is read from the end of the file, so browsing a large remote ZIP downloads only its directory.
- **Safety:** names containing `..`, absolute paths or drive letters are dropped (zip-slip). Every extracted member is checked against its recorded size and CRC-32 (ZIP; gzip trailer for `.tgz`). Encrypted members and other compression methods (bzip2, LZMA, zstd, deflate64) report a clear error. Local extraction writes an exclusive staging file beside the destination and publishes it without replacement.
- **Implementation:** own inflate decoder (no new dependency), indexes cached per archive (8 most recent, validated by size and mtime), at most 2M entries.

## Measurements

200 MB highly compressible text member, local, warm cache: `aicmd-cli copy perf.zip\big.txt out.txt` 0.81 s; Windows bsdtar `tar -xf` 0.25 s. The decoder is correct but about 3× slower than zlib; `.tgz` member extraction decompresses from the start of the stream (gzip has no random access).

## Not yet

- Packing / writing into archives (Alt+F5, F5 into an archive, delete inside).
- Other formats via **64-bit WCX plugins** (plain C ABI, reuses the TC ecosystem). 7z/RAR on Linux (p7zip `7z.so` or libarchive). Multi-volume RAR/7z. Batch extraction from solid archives in one pass.
- Archives inside archives, search inside archives, media playback from archives, faster inflate.
