# Fetches 7z.dll (7-Zip, GNU LGPL + unRAR restriction) from the official, pinned 7-Zip MSI and
# places it next to the build outputs. aicmd loads it at runtime to browse 7z, RAR, CAB, ISO,
# TAR.XZ and TAR.BZ2 archives. The DLL is redistributed unmodified; see third_party\7zip\README.md.
param([string]$Destination = "$PSScriptRoot\..\build")
$ErrorActionPreference = 'Stop'
$version = '2606'
$sha256 = '6dc822569e777d9a3ecd5ffcbc22b55600af87e6c5d9810a64e8566cf5a4af9f'
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$cache = Join-Path $workspace 'artifacts\7zip'
New-Item -ItemType Directory -Force $cache | Out-Null
$msi = Join-Path $cache "7z$version-x64.msi"
if (-not (Test-Path $msi)) { Invoke-WebRequest "https://www.7-zip.org/a/7z$version-x64.msi" -OutFile $msi -UseBasicParsing }
$actual = (Get-FileHash $msi -Algorithm SHA256).Hash.ToLower()
if ($actual -ne $sha256) { Remove-Item $msi; throw "7-Zip MSI hash mismatch: $actual" }
$extract = Join-Path $cache "x$version"
if (-not (Test-Path "$extract\Files\7-Zip\7z.dll")) {
    $p = Start-Process msiexec -Wait -PassThru -ArgumentList '/a',"`"$msi`"",'/qn',"TARGETDIR=`"$extract`""
    if ($p.ExitCode) { throw "msiexec /a failed with $($p.ExitCode)" }
}
New-Item -ItemType Directory -Force $Destination | Out-Null
Copy-Item "$extract\Files\7-Zip\7z.dll" $Destination -Force
Copy-Item "$extract\Files\7-Zip\License.txt" (Join-Path $workspace 'third_party\7zip\License.txt') -Force
Get-Item (Join-Path $Destination '7z.dll') | Select-Object FullName, Length, @{n='Version';e={$_.VersionInfo.ProductVersion}}
