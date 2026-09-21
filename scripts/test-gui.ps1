$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$artifacts = Join-Path $workspace 'artifacts'
[IO.Directory]::CreateDirectory($artifacts) | Out-Null
$report = Join-Path $artifacts ('gui-smoke-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss') + '.txt')
$process = Start-Process -FilePath "$workspace\build\aicmd.exe" -ArgumentList @('--smoke',('"'+$report+'"')) -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
if (-not $process.WaitForExit(25000)) {
    Stop-Process -Id $process.Id
    throw 'Native GUI smoke timed out'
}
if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $report)) { throw 'Native GUI smoke failed' }
$result = Get-Content -LiteralPath $report
if ($result -notmatch '^PASS ') { throw $result }
$result
