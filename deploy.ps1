[CmdletBinding()]
param(
    [string]$ConsoleHost = '',
    [int]$LoaderPort = 9021
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

$payload = Join-Path $projectRoot 'dist\bfplayer-standalone.elf'
if (-not (Test-Path -LiteralPath $payload)) {
    throw "Missing standalone payload: $payload. Run build.ps1 first."
}

$sender = Join-Path $workspaceRoot '.agents\skills\ps5_development\scripts\send_payload.py'
& python $sender $payload --host $ConsoleHost --port $LoaderPort
if ($LASTEXITCODE -ne 0) {
    throw 'Standalone payload injection failed.'
}

$logDir = Join-Path $workspaceRoot 'logs\deploy'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
[IO.File]::WriteAllLines(
    (Join-Path $logDir 'bfplayer-latest.log'),
    @(
        "deploy_utc=$([DateTime]::UtcNow.ToString('o'))",
        "host=$ConsoleHost",
        "port=$LoaderPort",
        "payload=$payload",
        "bytes=$((Get-Item -LiteralPath $payload).Length)",
        "sha256=$((Get-FileHash -Algorithm SHA256 -LiteralPath $payload).Hash.ToLowerInvariant())"
    ))

Write-Host 'Standalone Media Center launcher injected.'
