param(
    [string]$Root = "D:\pokemon-tcg-ai-battle4",
    [int]$FirstDay = 3,
    [int]$LastDay = 19
)

$ErrorActionPreference = "Stop"
$replayRoot = Join-Path $Root "data\kaggle_replays"
$source = Join-Path $Root "data\train-v3-all.jsonl"
$boundaryOutput = Join-Path $Root "data\exact-evaluator-v3-all.bin"
$generalOutput = Join-Path $Root "data\general-evaluator-v3-all.bin"
$sampleRoot = Join-Path $Root "sample_submission\sample_submission"
$dates = @($FirstDay..$LastDay | ForEach-Object { "2026-07-{0:D2}" -f $_ })

Write-Output "waiting for daily replay downloads"
while (@($dates | Where-Object {
    -not (Test-Path (Join-Path (Join-Path $replayRoot $_) ".kaggle-complete.json"))
}).Count -gt 0) {
    Start-Sleep -Seconds 30
}

Write-Output "extracting every complete replay"
$extractor = Join-Path $Root "tools\extract_replay_v3_dataset.py"
$partA = Join-Path $Root "data\train-v3-all.part-a.jsonl"
$partB = Join-Path $Root "data\train-v3-all.part-b.jsonl"
$procA = Start-Process -FilePath "python" -ArgumentList @($extractor, $replayRoot, $partA,
    "--mode", "both", "--limit", "0", "--max-replays", "0", "--progress-every", "1000",
    "--date-from", "2026-06-16", "--date-to", "2026-07-02") -RedirectStandardOutput `
    (Join-Path $Root "data\extract-part-a.stdout.log") -RedirectStandardError `
    (Join-Path $Root "data\extract-part-a.stderr.log") -PassThru -WindowStyle Hidden
$procB = Start-Process -FilePath "python" -ArgumentList @($extractor, $replayRoot, $partB,
    "--mode", "both", "--limit", "0", "--max-replays", "0", "--progress-every", "1000",
    "--date-from", "2026-07-03", "--date-to", "2026-07-19") -RedirectStandardOutput `
    (Join-Path $Root "data\extract-part-b.stdout.log") -RedirectStandardError `
    (Join-Path $Root "data\extract-part-b.stderr.log") -PassThru -WindowStyle Hidden
$procA.WaitForExit()
$procB.WaitForExit()
if ($procA.ExitCode -ne 0 -or $procB.ExitCode -ne 0) { throw "full replay extraction failed" }
$partManifestPaths = @("$partA.manifest.json", "$partB.manifest.json")
$partManifests = $partManifestPaths | ForEach-Object {
    Get-Content -LiteralPath $_ -Raw | ConvertFrom-Json
}
$utf8 = [System.Text.UTF8Encoding]::new($false)
$writer = [System.IO.StreamWriter]::new($source, $false, $utf8)
try {
    foreach ($part in @($partA, $partB)) {
        $reader = [System.IO.StreamReader]::new($part)
        try {
            while (($line = $reader.ReadLine()) -ne $null) {
                $writer.WriteLine($line)
            }
        } finally {
            $reader.Dispose()
        }
    }
} finally {
    $writer.Dispose()
}
Remove-Item -LiteralPath $partA,$partB -Force
$aggregate = [ordered]@{
    mode = "both"
    source = $source
    candidateReplays = ($partManifests.candidateReplays | Measure-Object -Sum).Sum
    examinedReplays = ($partManifests.examinedReplays | Measure-Object -Sum).Sum
    acceptedReplays = ($partManifests.acceptedReplays | Measure-Object -Sum).Sum
    rejectedReplays = ($partManifests.rejectedReplays | Measure-Object -Sum).Sum
    examples = ($partManifests.examples | Measure-Object -Sum).Sum
    featureSchemaVersion = 3
    informationSetSafe = $true
    boundaryOnly = $null
    lossWeighting = "one_total_weight_per_match"
}
$aggregate | ConvertTo-Json | Set-Content -LiteralPath "$source.manifest.json" -Encoding utf8
Remove-Item -LiteralPath $partManifestPaths -Force

Write-Output "training boundary evaluator on all boundary samples"
& python (Join-Path $Root "tools\train_v3_evaluator.py") $source $boundaryOutput `
    --initial-model (Join-Path $sampleRoot "exact-evaluator-v3.bin") `
    --sample-kind boundary --boundary-only --epochs 5
if ($LASTEXITCODE -ne 0) { throw "boundary training failed" }

Write-Output "training general evaluator on all intermediate samples"
& python (Join-Path $Root "tools\train_v3_evaluator.py") $source $generalOutput `
    --initial-model $boundaryOutput --sample-kind intermediate --epochs 5
if ($LASTEXITCODE -ne 0) { throw "general training failed" }

$boundaryReport = Get-Content -LiteralPath ($boundaryOutput -replace '\.bin$', '.report.json') -Raw |
    ConvertFrom-Json
if ($boundaryReport.improvesInitialMse) {
    Copy-Item -LiteralPath $boundaryOutput -Destination (Join-Path $sampleRoot "exact-evaluator-v3.bin") -Force
    Copy-Item -LiteralPath ($boundaryOutput -replace '\.bin$', '.report.json') `
        -Destination (Join-Path $sampleRoot "exact-evaluator-v3.report.json") -Force
} else {
    Write-Warning "boundary quantized regression; retaining deployed exact evaluator"
}
$generalReport = Get-Content -LiteralPath ($generalOutput -replace '\.bin$', '.report.json') -Raw |
    ConvertFrom-Json
if ($generalReport.improvesInitialMse) {
    Copy-Item -LiteralPath $generalOutput -Destination (Join-Path $sampleRoot "general-evaluator-v3.bin") -Force
    Copy-Item -LiteralPath ($generalOutput -replace '\.bin$', '.report.json') `
        -Destination (Join-Path $sampleRoot "general-evaluator-v3.report.json") -Force
} else {
    Write-Warning "general quantized regression; retaining deployed general evaluator"
}

[pscustomobject]@{
    completedUtc = [datetime]::UtcNow.ToString("o")
    source = $source
    boundaryModel = $boundaryOutput
    generalModel = $generalOutput
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Root "data\full-retrain-complete.json") -Encoding utf8
Write-Output "full retrain complete"
