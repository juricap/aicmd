param([string]$Build = "$PSScriptRoot\..\build")
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$artifactRoot = Join-Path $workspace 'artifacts'
[IO.Directory]::CreateDirectory($artifactRoot) | Out-Null
Add-Type -TypeDefinition @'
using System.IO;
public static class Fixtures {
    public static void Make(string path, int count) {
        Directory.CreateDirectory(path);
        for (int i = 0; i < count; ++i) {
            string file = Path.Combine(path, "entry-" + i.ToString("D6") + ".txt");
            if (!File.Exists(file)) using (File.Create(file)) {}
        }
    }
}
'@
foreach ($count in @(10000,100000)) {
    $fixture = Join-Path $artifactRoot "fixture-$count"
    [Fixtures]::Make($fixture,$count)
    $output = & "$Build\aicmd-cli.exe" bench $fixture 20
    if ($LASTEXITCODE) { throw "Benchmark failed: $output" }
    $output | Set-Content -Encoding UTF8 (Join-Path $artifactRoot "bench-$count.txt")
    $samples = @($output | ForEach-Object { if ($_ -match 'ms=([0-9.]+)') { [double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture) } } | Sort-Object)
    [pscustomobject]@{ Entries=$count; Runs=$samples.Count; MedianMs=$samples[10]; P95Ms=$samples[18] }
}
