# Builds a portable Windows x64 release zip under artifacts\release.
# libmpv (media playback) and jptxt (viewer/editor) are separate projects with
# their own licenses; they are bundled only when explicitly requested.
param(
    [switch]$NoBuild,
    [switch]$IncludeJptxt,
    [string]$JptxtPath = "$PSScriptRoot\..\..\jptxt\build\jptxt.exe",
    [switch]$IncludeMpv
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
Push-Location $workspace
try {
    if (-not $NoBuild) { cmd /c "`"$workspace\build.bat`" 2>&1" | Out-Host; if ($LASTEXITCODE) { throw 'Build or tests failed' } }
    $version = [regex]::Match((Get-Content CMakeLists.txt -Raw),'project\(aicmd VERSION ([0-9.]+)').Groups[1].Value
    if (-not $version) { throw 'Cannot read project version' }
    $name = "aicmd-$version-win-x64"
    $release = Join-Path $workspace 'artifacts\release'
    $stage = Join-Path $release $name
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    foreach ($exe in 'aicmd.exe','aicmd-cli.exe','aiview.exe','aiview-tool.exe') {
        $source = Join-Path 'build' $exe
        if (-not (Test-Path $source)) { throw "Missing build output $exe" }
        Copy-Item $source $stage
        $built = (Get-Item $source).VersionInfo.ProductVersion
        if ($built -ne $version) { throw "$exe reports version $built, expected $version" }
    }
    $notices = @("aicmd $version third-party notices", '', 'nlohmann/json 3.11.3 (MIT), used for session persistence:', '', (Get-Content third_party\nlohmann\LICENSE.MIT -Raw))
    # 7z.dll (7-Zip, LGPL + unRAR restriction) is redistributed unmodified for 7z/RAR/CAB/ISO/TAR.XZ browsing.
    if (-not (Test-Path build\7z.dll)) { & "$PSScriptRoot\setup-7zip.ps1" | Out-Host }
    Copy-Item build\7z.dll $stage
    Copy-Item third_party\7zip\License.txt (Join-Path $stage '7z-License.txt')
    $notices += @('', '7-Zip 26.06 7z.dll (C) Igor Pavlov: GNU LGPL with the unRAR license restriction; BSD 3-clause for some code.', 'Unmodified binary from the official 7-Zip MSI; full license in 7z-License.txt; source code at https://www.7-zip.org/.', 'You may replace 7z.dll with any compatible build.')
    if ($IncludeMpv) {
        if (-not (Test-Path build\libmpv-2.dll)) { throw 'build\libmpv-2.dll missing; run scripts\setup-aiview.ps1' }
        Write-Warning 'Bundling libmpv: audit its GPL/LGPL obligations (see third_party\mpv\README.md) before publishing.'
        Copy-Item build\libmpv-2.dll $stage
        $notices += @('', 'libmpv (shinchiro mpv-winbuild-cmake 20260921, mpv e76a35ec95). See https://github.com/shinchiro/mpv-winbuild-cmake for licenses and sources.', '', (Get-Content third_party\mpv\Copyright -Raw))
    }
    if ($IncludeJptxt) {
        if (-not (Test-Path $JptxtPath)) { throw "jptxt not found at $JptxtPath" }
        Copy-Item $JptxtPath (Join-Path $stage 'jptxt.exe')
    }
    if (Test-Path LICENSE) { Copy-Item LICENSE $stage } else { Write-Warning 'No LICENSE file in the repository yet; this package is not ready for public release.' }
    Set-Content -Encoding utf8 (Join-Path $stage 'THIRD-PARTY-NOTICES.txt') ($notices -join "`r`n")
    $media = if ($IncludeMpv) { 'Space opens images, video and audio in aiview.' } else { 'Media playback is optional: place libmpv-2.dll beside aiview.exe (see the project README). Without it, Space marks entries.' }
    $editor = if ($IncludeJptxt) { 'F3/F4 open the bundled jptxt viewer/editor.' } else { 'F3/F4 use jptxt when found (beside aicmd.exe, on PATH, or AICMD_EDITOR); otherwise the associated Windows application.' }
    Set-Content -Encoding utf8 (Join-Path $stage 'README.txt') @"
aicmd $version - compact, keyboard-first two-panel file commander for Windows

Run aicmd.exe. No installation is required; the session is stored in
%LOCALAPPDATA%\aicmd\session.json. Press F1 for keyboard shortcuts.

  Tab switch panel   F5 copy   F6 move   Shift+F6 rename   F7 new folder
  F8/Delete Recycle Bin   Shift+F8 permanent delete   Ctrl+L path / ssh://alias/path
  Alt+F1/F2 drives and SSH hosts (from ~/.ssh/config, keys handled by OpenSSH)

Existing files are never overwritten; conflicting items fail and are reported.
Enter starts programs (.exe, .bat, .msi, shortcuts) and opens archives as read-only
folders: ZIP/JAR, TAR/TGZ built in; 7Z, RAR, CAB, ISO, TAR.XZ, TAR.BZ2 via the bundled 7z.dll.
$editor
$media

aicmd-cli.exe lists folders and copies files from scripts: aicmd-cli.exe commands
"@
    # The packaged GUI must start and pass its smoke test outside the build tree.
    $report = Join-Path $release "$name-smoke.txt"
    $smoke = Start-Process (Join-Path $stage 'aicmd.exe') -ArgumentList @('--smoke',('"'+$report+'"')) -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    if (-not $smoke.WaitForExit(30000)) { Stop-Process -Id $smoke.Id; throw 'Packaged GUI smoke timed out' }
    if ($smoke.ExitCode -ne 0 -or (Get-Content $report) -notmatch '^PASS ') { throw "Packaged GUI smoke failed: $(Get-Content $report -ErrorAction SilentlyContinue)" }
    Remove-Item "$report*" -Force
    & (Join-Path $stage 'aicmd-cli.exe') list $stage | Out-Null
    if ($LASTEXITCODE) { throw 'Packaged CLI failed' }
    $zip = Join-Path $release "$name.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $stage -DestinationPath $zip
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
    Set-Content -Encoding ascii "$zip.sha256" "$hash  $name.zip"
    Get-Item $zip | Select-Object FullName,Length
    "SHA256 $hash"
} finally { Pop-Location }
