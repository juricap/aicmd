param([string]$SevenZip = 'C:\Program Files\7-Zip\7z.exe')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$version = '20260921'
$asset = 'mpv-dev-x86_64-20260921-git-e76a35ec95.7z'
$expected = '361392D64A231830AC79D1C0F91DDB68C6A709ACCD282B2C87B13889D382FDEC'
$cache = Join-Path $workspace 'artifacts\mpv'
[IO.Directory]::CreateDirectory($cache) | Out-Null
$archive = Join-Path $cache $asset
if (-not (Test-Path -LiteralPath $archive)) {
    & curl.exe -L --fail -o $archive "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/$version/$asset"
    if ($LASTEXITCODE) { throw 'libmpv download failed' }
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'libmpv archive checksum mismatch' }
if (-not (Test-Path -LiteralPath $SevenZip)) { throw '7-Zip is required; pass -SevenZip <7z.exe>' }
& $SevenZip x $archive "-o$cache\runtime" -y | Out-Null
if ($LASTEXITCODE) { throw 'libmpv extraction failed' }
[IO.Directory]::CreateDirectory((Join-Path $workspace 'build')) | Out-Null
Copy-Item -LiteralPath "$cache\runtime\libmpv-2.dll" -Destination "$workspace\build\libmpv-2.dll"
'Installed pinned libmpv runtime for local development. See third_party/mpv/README.md for provenance and distribution limits.'
