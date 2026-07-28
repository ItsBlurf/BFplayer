[CmdletBinding()]
param(
    [string]$ZipPath = (Join-Path $PSScriptRoot '..\dist\BFplayer-websrv.zip')
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$resolvedZip = (Resolve-Path -LiteralPath $ZipPath).Path
$expectedPayloadFiles = @(
    'eboot.elf',
    'assets/fonts/NotoSans-Regular.ttf',
    'assets/fonts/OFL.txt',
    'sce_sys/icon0.png',
    'INSTALL.md',
    'LICENSE',
    'THIRD_PARTY_NOTICES.md'
) | Sort-Object
$expectedArchiveFiles = @(
    $expectedPayloadFiles + @('build-manifest.json') |
        ForEach-Object { "BFplayer/$_" } |
        Sort-Object
)

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($resolvedZip)
try {
    $archiveFiles = @(
        $archive.Entries |
            Where-Object { $_.Name } |
            ForEach-Object { $_.FullName.Replace('\', '/') } |
            Sort-Object
    )
    $difference = Compare-Object $expectedArchiveFiles $archiveFiles
    if ($difference) {
        throw "Unexpected websrv ZIP inventory: $($difference | Out-String)"
    }
}
finally {
    $archive.Dispose()
}

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$verifyRoot = Join-Path $temporaryRoot ("BFplayer-websrv-" + [Guid]::NewGuid().ToString('N'))
$resolvedVerifyRoot = [IO.Path]::GetFullPath($verifyRoot)
if (-not $resolvedVerifyRoot.StartsWith(
        $temporaryRoot,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to use a verification folder outside the system temporary directory.'
}

try {
    [IO.Compression.ZipFile]::ExtractToDirectory($resolvedZip, $resolvedVerifyRoot)
    $appRoot = Join-Path $resolvedVerifyRoot 'BFplayer'
    $manifestPath = Join-Path $appRoot 'build-manifest.json'
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    $version = (Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'VERSION')).Trim()

    if ($manifest.name -ne 'BFplayer' -or
        $manifest.version -ne $version -or
        $manifest.target -ne 'x86_64-sie-ps5' -or
        $manifest.entrypoint -ne 'BFplayer/eboot.elf' -or
        $manifest.websrv_required -ne $true -or
        $manifest.resident_bfplayer_launcher -ne $false) {
        throw 'The websrv build manifest does not describe the expected package.'
    }

    $manifestFiles = @($manifest.files.path | Sort-Object)
    $manifestDifference = Compare-Object $expectedPayloadFiles $manifestFiles
    if ($manifestDifference) {
        throw "Unexpected websrv manifest inventory: $($manifestDifference | Out-String)"
    }

    foreach ($file in $manifest.files) {
        $path = Join-Path $appRoot ($file.path -replace '/', [IO.Path]::DirectorySeparatorChar)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Manifest file is missing: $($file.path)"
        }
        $item = Get-Item -LiteralPath $path
        if ($item.Length -ne [int64]$file.bytes) {
            throw "Manifest size mismatch: $($file.path)"
        }
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
        if ($hash -ne $file.sha256) {
            throw "Manifest hash mismatch: $($file.path)"
        }
    }

    $ebootPath = Join-Path $appRoot 'eboot.elf'
    $ebootHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ebootPath).Hash.ToLowerInvariant()
    if ($ebootHash -ne $manifest.player_sha256) {
        throw 'eboot.elf does not match player_sha256.'
    }
    $stream = [IO.File]::OpenRead($ebootPath)
    try {
        $header = New-Object byte[] 20
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length) {
            throw 'eboot.elf is too short.'
        }
    }
    finally {
        $stream.Dispose()
    }
    if ($header[0] -ne 0x7f -or
        $header[1] -ne 0x45 -or
        $header[2] -ne 0x4c -or
        $header[3] -ne 0x46 -or
        $header[4] -ne 2 -or
        $header[5] -ne 1 -or
        $header[7] -ne 9 -or
        $header[16] -ne 3 -or
        $header[17] -ne 0 -or
        $header[18] -ne 0x3e -or
        $header[19] -ne 0) {
        throw 'eboot.elf is not a FreeBSD x86-64 DYN ELF.'
    }

    $iconPath = Join-Path $appRoot 'sce_sys\icon0.png'
    $iconStream = [IO.File]::OpenRead($iconPath)
    try {
        $pngHeader = New-Object byte[] 8
        if ($iconStream.Read($pngHeader, 0, $pngHeader.Length) -ne $pngHeader.Length -or
            [BitConverter]::ToString($pngHeader) -ne '89-50-4E-47-0D-0A-1A-0A') {
            throw 'sce_sys/icon0.png is not a PNG file.'
        }
    }
    finally {
        $iconStream.Dispose()
    }

    [pscustomobject]@{
        Package = $resolvedZip
        Version = $manifest.version
        Files = $expectedArchiveFiles.Count
        EbootBytes = (Get-Item -LiteralPath $ebootPath).Length
        EbootSHA256 = $ebootHash
        ZipSHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $resolvedZip).Hash.ToLowerInvariant()
        Result = 'PASS'
    }
}
finally {
    if (Test-Path -LiteralPath $resolvedVerifyRoot) {
        Remove-Item -LiteralPath $resolvedVerifyRoot -Recurse -Force
    }
}
