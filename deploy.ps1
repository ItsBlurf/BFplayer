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

$dist = Join-Path $projectRoot 'dist'
$files = @(
    @{ Local = 'assets\fonts\NotoSans-Regular.ttf'; Remote = 'assets/fonts/NotoSans-Regular.ttf' },
    @{ Local = 'assets\fonts\OFL.txt'; Remote = 'assets/fonts/OFL.txt' },
    @{ Local = 'sce_sys\icon0.png'; Remote = 'sce_sys/icon0.png' },
    @{ Local = 'build-manifest.json'; Remote = 'build-manifest.json' },
    @{ Local = 'ps5-media-center.elf'; Remote = 'ps5-media-center.elf' },
    @{ Local = 'homebrew.js'; Remote = 'homebrew.js' },
    @{ Local = 'THIRD_PARTY_NOTICES.md'; Remote = 'THIRD_PARTY_NOTICES.md' }
)

$logLines = @(
    "deploy_utc=$([DateTime]::UtcNow.ToString('o'))",
    "host=$ConsoleHost",
    "port=$FtpPort"
)
foreach ($file in $files) {
    $local = Join-Path $dist $file.Local
    if (-not (Test-Path -LiteralPath $local)) {
        throw "Missing package file: $local. Run build.ps1 first."
    }
    $remote = "ftp://${ConsoleHost}:$FtpPort/data/homebrew/PS5-MediaCenter/$($file.Remote)"
    & curl.exe --fail --silent --show-error --ftp-create-dirs --upload-file $local $remote
    if ($LASTEXITCODE -ne 0) {
        throw "Upload failed: $($file.Local)"
    }
    $logLines += "uploaded=$($file.Remote) bytes=$((Get-Item -LiteralPath $local).Length)"
    Write-Host "Uploaded $($file.Remote)"
}

$logDir = Join-Path $workspaceRoot 'logs\deploy'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
[IO.File]::WriteAllLines(
    (Join-Path $logDir 'ps5-media-center-latest.log'),
    $logLines)

Write-Host 'Deployment complete. Open websrv and launch PS5 Media Center through HBL.'
