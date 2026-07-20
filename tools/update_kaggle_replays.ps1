param(
    [string]$Root = "D:\pokemon-tcg-ai-battle4\data\kaggle_replays",
    [int]$Worker = -1,
    [int]$Workers = 2,
    [int]$FirstDay = 3,
    [int]$LastDay = 19
)

$ErrorActionPreference = "Stop"
$env:PYTHONIOENCODING = "utf-8"

function Get-KaggleJsonCount([string]$Date) {
    $dataset = "kaggle/pokemon-tcg-ai-battle-episodes-$Date"
    $token = $null
    $count = 0
    for ($page = 1; $page -le 100; $page++) {
        $arguments = @("datasets", "files", $dataset, "--csv", "--page-size", "200")
        if ($token) { $arguments += @("--page-token", $token) }
        $output = & kaggle @arguments 2>&1
        $count += @($output | Where-Object {
            $_ -match '^[^,]+\.json,[0-9]+,'
        }).Count
        $next = $output | Select-String '^Next Page Token = ' | Select-Object -First 1
        if (-not $next) { return $count }
        $token = $next.ToString() -replace '^Next Page Token = ', ''
    }
    throw "Kaggle file listing exceeded 100 pages for $Date"
}

New-Item -ItemType Directory -Force -Path $Root | Out-Null
$dates = @($FirstDay..$LastDay | ForEach-Object { "2026-07-{0:D2}" -f $_ })
if ($Worker -ge 0) {
    $dates = @($dates | Where-Object {
        $index = $dates.IndexOf($_)
        ($index % [math]::Max(1, $Workers)) -eq $Worker
    })
}

foreach ($date in $dates) {
    $target = Join-Path $Root $date
    $marker = Join-Path $target ".kaggle-complete.json"
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    $expected = Get-KaggleJsonCount $date
    $archive = Join-Path $target "pokemon-tcg-ai-battle-episodes-$date.zip"
    $actual = @(Get-ChildItem -LiteralPath $target -File -Filter "*.json" -ErrorAction SilentlyContinue).Count
    if ((Test-Path -LiteralPath $archive) -and (Get-Item -LiteralPath $archive).Length -gt 0) {
        [pscustomobject]@{
            date = $date
            expected = $expected
            json = $actual
            archive = $archive
            dataset = "kaggle/pokemon-tcg-ai-battle-episodes-$date"
            completedUtc = [datetime]::UtcNow.ToString("o")
        } | ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding utf8
        Write-Output "$date complete archive expectedJson=$expected extractedJson=$actual"
        continue
    }
    if (Test-Path -LiteralPath $marker) {
        $saved = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
        if ($saved.expected -eq $expected -and $actual -eq $expected) {
            Write-Output "$date complete json=$actual"
            continue
        }
    }
    Write-Output "$date downloading expected=$expected actual=$actual"
    & kaggle datasets download -d "kaggle/pokemon-tcg-ai-battle-episodes-$date" -p $target -q
    if ($LASTEXITCODE -ne 0) { throw "Kaggle download failed for $date" }
    if (-not (Test-Path -LiteralPath $archive) -or (Get-Item -LiteralPath $archive).Length -le 0) {
        throw "Kaggle archive was not downloaded for $date"
    }
    $actual = @(Get-ChildItem -LiteralPath $target -File -Filter "*.json" -ErrorAction SilentlyContinue).Count
    [pscustomobject]@{
        date = $date
        expected = $expected
        json = $actual
        archive = $archive
        dataset = "kaggle/pokemon-tcg-ai-battle-episodes-$date"
        completedUtc = [datetime]::UtcNow.ToString("o")
    } | ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding utf8
    Write-Output "$date complete json=$actual"
}
