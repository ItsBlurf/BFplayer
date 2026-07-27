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
    @{
        Name = 'standalone-launcher.log'
        Remote = '/data/BFplayer/standalone-launcher.log'
        Required = $true
    },
    @{
        Name = 'player-stdio.log'
        Remote = '/data/BFplayer/player-stdio.log'
        Required = $false
    },
    @{
        Name = 'latest.log'
        Remote = '/data/BFplayer/logs/latest.log'
        Required = $false
    },
    @{
        Name = 'previous.log'
        Remote = '/data/BFplayer/logs/previous.log'
        Required = $false
    }
)
$summary = @(
    "collected_utc=$([DateTime]::UtcNow.ToString('o'))",
    "host=$ConsoleHost",
    "port=$FtpPort"
)
$downloaded = @()

foreach ($file in $files) {
    $local = Join-Path $OutputDirectory $file.Name
    $remote = "ftp://$($ConsoleHost):$FtpPort$($file.Remote)"
    & curl.exe --fail --silent --show-error --ftp-method nocwd --output $local $remote
    if ($LASTEXITCODE -ne 0) {
        if ($file.Required) {
            throw "Required diagnostic download failed: $($file.Name)"
        }
        Write-Warning "Optional diagnostic log was unavailable: $($file.Name)"
        continue
    }
    $item = Get-Item -LiteralPath $local
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $local).Hash.ToLowerInvariant()
    $summary += "file=$($file.Name) bytes=$($item.Length) sha256=$hash"
    $downloaded += $local
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

$analysis = @(
    "generated_utc=$([DateTime]::UtcNow.ToString('o'))",
    "downloaded_logs=$($downloaded.Count)"
)
$patterns = @(
    'start version=',
    'request route=/launch',
    'launch result=',
    'launch coalesced',
    'BFPLAYER_BOOT_STAGE',
    'boot-stage stage=',
    'application-start build=',
    'video-source ',
    'playback-format ',
    'playback-heartbeat ',
    'BFPLAYER_FATAL_SIGNAL',
    'application-end '
)
foreach ($local in $downloaded) {
    $analysis += ''
    $analysis += "file=$([IO.Path]::GetFileName($local))"
    foreach ($pattern in $patterns) {
        $matches = @(Select-String -LiteralPath $local -SimpleMatch $pattern)
        $analysis += "marker=$pattern count=$($matches.Count)"
        if ($matches.Count -gt 0) {
            $analysis += "last=$($matches[-1].Line)"
        }
    }
    $analysis += 'tail-begin'
    $analysis += @(Get-Content -LiteralPath $local -Tail 40)
    $analysis += 'tail-end'
}
[IO.File]::WriteAllLines(
    (Join-Path $OutputDirectory 'diagnostic-summary.txt'),
    $analysis)
Write-Host "Diagnostics saved to $OutputDirectory"
