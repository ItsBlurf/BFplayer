[CmdletBinding()]
param(
    [string]$ConsoleHost = '',
    [int]$FtpPort = 2121
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$workspaceRoot = (Resolve-Path (Join-Path $projectRoot '..\..')).Path
. (Join-Path $workspaceRoot 'ps5dev-env.ps1')

if (-not $ConsoleHost) {
    $ConsoleHost = $env:PS5_HOST
}
if (-not $ConsoleHost) {
    throw 'No console host was supplied and PS5_HOST is empty.'
}

$corpus = Join-Path $workspaceRoot 'payloads\test\bfplayer'
$files = [ordered]@{
    'bfplayer-multitrack-test.mkv' = 'bcdd343024479a1ab631c1385363a9b17b00ede4bd70d499150745f5c76983de'
    'bfplayer-multitrack-test.en.srt' = 'f8d4d77521274aaa5b6603e7cb740e644eb7ea85b3f08132197730484978e796'
    'bfplayer-multitrack-test.styled.ass' = 'f6340733393d3e222f50eaa3c947597c980b9c3669d4f0e6f5d166fca302cf3e'
    'bfplayer-multitrack-test.bitmap.sup' = 'f03e72bbd34177046be57da8d760718a078d139126d073e9e28febdd9624b7d9'
    'ffmpeg-pgs-supsample.mkv' = 'e6c8f93f57d0371603704d7e7b16933e6c4c5df669da42b42a2a84de881e0f27'
    'bfplayer-multitrack-test.png' = '2b5f1f1c9e21ef5ec8170bd4a081f69623b39fe439fe834b9bde1f4a5a32d5ae'
}

foreach ($item in $files.GetEnumerator()) {
    $path = if ($item.Key -eq 'bfplayer-multitrack-test.png') {
        Join-Path $projectRoot 'assets\icon0.png'
    } else {
        Join-Path $corpus $item.Key
    }
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing test corpus file: $path"
    }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    if ($actual -ne $item.Value) {
        throw "Test corpus hash mismatch: $($item.Key)"
    }
}

$tcp = [Net.Sockets.TcpClient]::new()
try {
    $connection = $tcp.ConnectAsync($ConsoleHost, $FtpPort)
    if (-not $connection.Wait(2500) -or -not $tcp.Connected) {
        throw "PS5 FTP is not reachable at ${ConsoleHost}:$FtpPort"
    }
}
finally {
    $tcp.Dispose()
}

$logLines = @(
    "stage_utc=$([DateTime]::UtcNow.ToString('o'))",
    "host=$ConsoleHost",
    "port=$FtpPort",
    'remote=/data/media/BFPLAYER-Test'
)
foreach ($item in $files.GetEnumerator()) {
    $local = if ($item.Key -eq 'bfplayer-multitrack-test.png') {
        Join-Path $projectRoot 'assets\icon0.png'
    } else {
        Join-Path $corpus $item.Key
    }
    $remote = "ftp://${ConsoleHost}:$FtpPort/data/media/BFPLAYER-Test/$($item.Key)"
    & curl.exe --fail --silent --show-error --ftp-create-dirs --upload-file $local $remote
    if ($LASTEXITCODE -ne 0) {
        throw "Test-media upload failed: $($item.Key)"
    }
    $logLines += "uploaded=$($item.Key) bytes=$((Get-Item -LiteralPath $local).Length) sha256=$($item.Value)"
    Write-Host "Uploaded $($item.Key)"
}

$logDir = Join-Path $workspaceRoot 'logs\deploy'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
[IO.File]::WriteAllLines(
    (Join-Path $logDir 'bfplayer-test-corpus-latest.log'),
    $logLines)

Write-Host 'Test corpus staged at /data/media/BFPLAYER-Test.'
