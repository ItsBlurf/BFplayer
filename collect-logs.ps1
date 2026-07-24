[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ConsoleHost,
    [int]$FtpPort = 2121,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot (
        'diagnostics\session-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$files = @(
    @{ Name = 'latest.log'; Required = $true },
    @{ Name = 'previous.log'; Required = $false }
)
$summary = @(
    "collected_utc=$([DateTime]::UtcNow.ToString('o'))",
    "host=$ConsoleHost",
    "port=$FtpPort"
)

foreach ($file in $files) {
    $local = Join-Path $OutputDirectory $file.Name
    $remote = "ftp://$($ConsoleHost):$FtpPort/data/PS5-MediaCenter/logs/$($file.Name)"
    & curl.exe --fail --silent --show-error --ftp-method nocwd --output $local $remote
    if ($LASTEXITCODE -ne 0) {
        if ($file.Required) {
            throw "Required diagnostic download failed: $($file.Name)"
        }
        Write-Warning "No previous diagnostic log was available."
        continue
    }
    $item = Get-Item -LiteralPath $local
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $local).Hash.ToLowerInvariant()
    $summary += "file=$($file.Name) bytes=$($item.Length) sha256=$hash"
    Write-Host "Downloaded $($file.Name) ($($item.Length) bytes)"
}

[IO.File]::WriteAllLines(
    (Join-Path $OutputDirectory 'collection.txt'),
    $summary)
$manifest = Join-Path $projectRoot 'dist\build-manifest.json'
if (Test-Path -LiteralPath $manifest) {
    Copy-Item -LiteralPath $manifest -Destination (Join-Path $OutputDirectory 'build-manifest.json') -Force
    Write-Host 'Copied matching local build-manifest.json'
}
Write-Host "Diagnostics saved to $OutputDirectory"
