# 7-Zip (7z.dll)

aicmd loads `7z.dll` at runtime (it is not linked) to read 7z, RAR, CAB, ISO, ARJ, LZH, WIM,
TAR.XZ and TAR.BZ2 archives. Release packages ship the unmodified DLL from the official
7-Zip 26.06 x64 MSI (`scripts\setup-7zip.ps1`, SHA-256 pinned).

- Copyright (C) Igor Pavlov. License: GNU LGPL, with the unRAR license restriction for the RAR
  decoder and BSD 3-clause for some code. Full text: [License.txt](License.txt).
- Source code: https://www.7-zip.org/ (download page, "7-Zip Source code").
- The unRAR restriction forbids using that code to create a RAR-compatible archiver; aicmd only
  reads archives.
- You may replace the bundled `7z.dll` with any compatible build. Without a DLL next to
  `aicmd.exe`, aicmd uses an installed 7-Zip (registry `HKLM\SOFTWARE\7-Zip`, or
  `%ProgramFiles%\7-Zip`).

The COM interface declarations in `src/fs/sevenzip.cpp` are written from the documented 7-Zip
interface (IInArchive and friends); no 7-Zip source is compiled into aicmd.
