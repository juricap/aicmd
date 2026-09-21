param([string]$RemoteDirectory = '/data/devel/aicmd-check-20260921')
$ErrorActionPreference = 'Stop'
if ($RemoteDirectory -notmatch '^/data/devel/aicmd-check-[a-zA-Z0-9_-]+$') { throw 'Use an isolated /data/devel/aicmd-check-* directory.' }
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
Push-Location $workspace
try {
    [IO.Directory]::CreateDirectory((Join-Path $workspace 'artifacts')) | Out-Null
    tar -czf artifacts/aicmd-source.tar.gz CMakeLists.txt src tests third_party
    if ($LASTEXITCODE) { throw 'Source archive failed' }
    ssh ai "mkdir -p $RemoteDirectory"
    if ($LASTEXITCODE) { throw 'Remote directory creation failed' }
    scp artifacts/aicmd-source.tar.gz "ai:${RemoteDirectory}/source.tar.gz"
    if ($LASTEXITCODE) { throw 'Source transfer failed' }
    ssh ai "tar -xzf $RemoteDirectory/source.tar.gz -C $RemoteDirectory"
    if ($LASTEXITCODE) { throw 'Remote extraction failed' }
    ssh ai "cmake -S $RemoteDirectory -B $RemoteDirectory/build -DCMAKE_BUILD_TYPE=Release"
    if ($LASTEXITCODE) { throw 'Linux configure failed' }
    ssh ai "cmake --build $RemoteDirectory/build -j 2"
    if ($LASTEXITCODE) { throw 'Linux build failed' }
    ssh ai "ctest --test-dir $RemoteDirectory/build --output-on-failure"
    if ($LASTEXITCODE) { throw 'Linux tests failed' }
} finally { Pop-Location }
