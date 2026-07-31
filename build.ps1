[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$SdlVideoOverrideSource = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$workspaceRoot = (Resolve-Path (Join-Path $projectRoot '..\..')).Path
. (Join-Path $workspaceRoot 'ps5dev-env.ps1')

if (-not $env:PS5_PAYLOAD_SDK) {
    throw 'PS5_PAYLOAD_SDK was not initialized.'
}

$pacbrewHome = $env:PS5_PACBREW_HOME
if (-not $pacbrewHome) {
    $toolchains = Split-Path $env:PS5_PAYLOAD_SDK -Parent
    $pacbrewHome = Join-Path $toolchains 'pacbrew-v0.37\homebrew'
}
$pacbrewHome = (Resolve-Path -LiteralPath $pacbrewHome).Path
$usingCustomSdlVideoOverride = [bool]$SdlVideoOverrideSource
if ($usingCustomSdlVideoOverride) {
    $sdlVideoOverrideSource = (
        Resolve-Path -LiteralPath $SdlVideoOverrideSource
    ).Path
} else {
    $sdlVideoOverrideSource = Join-Path $projectRoot (
        'vendor\SDL_ps5_backend\lib\SDL_ps5video.c.o')
    $sdlVideoOverrideHashFile = "$sdlVideoOverrideSource.sha256"
    if (-not (Test-Path -LiteralPath $sdlVideoOverrideSource) -or
        -not (Test-Path -LiteralPath $sdlVideoOverrideHashFile)) {
        throw 'Missing PS5 SDL video override. Run tools\rebuild-sdl-ps5-video.ps1.'
    }
    $expectedSdlVideoOverrideHash = ((
        Get-Content -Raw -LiteralPath $sdlVideoOverrideHashFile
    ).Trim() -split '\s+')[0].ToLowerInvariant()
    $actualSdlVideoOverrideHash = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $sdlVideoOverrideSource
    ).Hash.ToLowerInvariant()
    if ($actualSdlVideoOverrideHash -ne $expectedSdlVideoOverrideHash) {
        throw 'PS5 SDL video override hash mismatch. Rebuild it before linking.'
    }
}

$required = @(
    'include\SDL2\SDL.h',
    'include\kitchensink2\kitchensink.h',
    'include\SDL2\SDL_image.h',
    'include\tinyxml2.h',
    'lib\libSDL2.a',
    'lib\libSDL2_image.a',
    'lib\libavformat.a',
    'lib\libass.a',
    'lib\libtinyxml2.a'
)
foreach ($item in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $pacbrewHome $item))) {
        throw "PacBrew v0.37 is incomplete: missing $item in $pacbrewHome"
    }
}

$compiler = Join-Path $workspaceRoot 'tools\bin\ps5-c++.cmd'
$linker = Join-Path $workspaceRoot 'tools\bin\ps5-clang.cmd'
$buildDir = Join-Path $projectRoot 'build\ps5'
$distDir = Join-Path $projectRoot 'dist'
$packageRoot = Join-Path $projectRoot 'build\package'
$packageDir = Join-Path $packageRoot 'BFplayer-standalone'
$websrvPackageDir = Join-Path $packageRoot 'BFplayer'
$websrvVerifyDir = Join-Path $packageRoot 'BFplayer-websrv-verify'
New-Item -ItemType Directory -Force -Path $buildDir, $distDir | Out-Null
$legacyLowerStem = 'ps5-' + 'media-' + 'center'
$legacyUpperStem = 'PS5-' + 'Media' + 'Center'
$obsoleteDistEntries = @(
    'homebrew.js',
    'BFplayer-direct-tile.zip',
    'BFplayer-direct-tile.zip.sha256',
    'BFplayer-websrv.zip',
    'BFplayer-websrv.zip.sha256',
    "$legacyLowerStem.elf",
    "$legacyLowerStem.sha256",
    "$legacyLowerStem-standalone.elf",
    "$legacyLowerStem-standalone.sha256",
    "$legacyUpperStem-standalone.zip",
    "$legacyUpperStem-standalone.zip.sha256",
    'bfplayer-tile-installer.elf',
    'bfplayer-tile-installer.sha256',
    'THIRD_PARTY_NOTICES.md',
    'assets',
    'sce_sys',
    'BFplayer-websrv'
)
$distPrefix = [IO.Path]::GetFullPath($distDir) + [IO.Path]::DirectorySeparatorChar
foreach ($entry in $obsoleteDistEntries) {
    $obsoletePath = [IO.Path]::GetFullPath((Join-Path $distDir $entry))
    if (-not $obsoletePath.StartsWith($distPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove obsolete output outside $distDir"
    }
    if (Test-Path -LiteralPath $obsoletePath) {
        Remove-Item -LiteralPath $obsoletePath -Recurse -Force
    }
}
$version = (Get-Content -Raw (Join-Path $projectRoot 'VERSION')).Trim()
$versionHeader = Join-Path $buildDir 'bfplayer_version.h'
$versionHeaderRelative = 'build\ps5\bfplayer_version.h'
[IO.File]::WriteAllText(
    $versionHeader,
    "#pragma once`n#define BFPLAYER_VERSION `"$version`"`n")

$common = @(
    '-std=c++20',
    '-Wall',
    '-Wextra',
    '-Wpedantic',
    '-DBFPLAYER_PS5=1',
    '-Ivendor\SDL_kitchensink\include',
    "-I$($pacbrewHome)\include",
    "-I$($pacbrewHome)\include\SDL2",
    '-Iinclude',
    '-include',
    $versionHeaderRelative
)
if ($Configuration -eq 'Release') {
    $common += @('-O2', '-DNDEBUG')
} else {
    $common += @('-O0', '-g3')
}

Push-Location $projectRoot
try {
    $sources = @(
        'src\main.cpp',
        'src\core\artwork.cpp',
        'src\core\bulk_import.cpp',
        'src\core\diagnostics.cpp',
        'src\core\dlna_protocol.cpp',
        'src\core\hdr_tonemap.cpp',
        'src\core\library_scanner.cpp',
        'src\core\library_view.cpp',
        'src\core\list_navigation.cpp',
        'src\core\media_sources.cpp',
        'src\core\player_settings.cpp',
        'src\core\playlist.cpp',
        'src\core\remote_control.cpp',
        'src\core\safe_read_file.cpp',
        'src\core\source_uri.cpp',
        'src\core\subtitle_browser.cpp',
        'src\core\subtitle_provider.cpp',
        'src\core\video_layout.cpp',
        'src\core\video_thumbnail.cpp',
        'src\network\dlna_client.cpp',
        'src\core\library_database.cpp',
        'src\core\media_probe.cpp',
        'src\playback\external_subtitles.cpp',
        'src\ui\library_ui.cpp',
        'src\ui\playback_osd.cpp'
    )
    $objects = @()
    foreach ($source in $sources) {
        $object = 'build\ps5\' + ([IO.Path]::GetFileNameWithoutExtension($source)) + '.o'
        & $compiler @common -c $source -o $object
        if ($LASTEXITCODE -ne 0) {
            throw "Compilation failed: $source"
        }
        $objects += $object
    }
    $cSources = @(
        'src\playback\kitsubimage_safe.c',
        'src\playback\kitchensink_audio_clock.c',
        'src\playback\kitchensink_subtitle_timing.c',
        'vendor\SDL_kitchensink\src\kiterror.c',
        'vendor\SDL_kitchensink\src\kitformat.c',
        'vendor\SDL_kitchensink\src\kitlib.c',
        'vendor\SDL_kitchensink\src\kitplayer.c',
        'vendor\SDL_kitchensink\src\kitsource.c',
        'vendor\SDL_kitchensink\src\kitutils.c',
        'vendor\SDL_kitchensink\src\internal\kitdecoder.c',
        'vendor\SDL_kitchensink\src\internal\kitdecoderthread.c',
        'vendor\SDL_kitchensink\src\internal\kitdemuxer.c',
        'vendor\SDL_kitchensink\src\internal\kitdemuxerthread.c',
        'vendor\SDL_kitchensink\src\internal\kitlibstate.c',
        'vendor\SDL_kitchensink\src\internal\kitpacketbuffer.c',
        'vendor\SDL_kitchensink\src\internal\kittimer.c',
        'vendor\SDL_kitchensink\src\internal\libass.c',
        'vendor\SDL_kitchensink\src\internal\audio\kitaudio.c',
        'vendor\SDL_kitchensink\src\internal\audio\kitaudioutils.c',
        'vendor\SDL_kitchensink\src\internal\subtitle\kitatlas.c',
        'vendor\SDL_kitchensink\src\internal\subtitle\kitsubtitle.c',
        'vendor\SDL_kitchensink\src\internal\subtitle\kitsubtitlepacket.c',
        'vendor\SDL_kitchensink\src\internal\subtitle\renderers\kitsubass.c',
        'vendor\SDL_kitchensink\src\internal\subtitle\renderers\kitsubrenderer.c',
        'vendor\SDL_kitchensink\src\internal\utils\kithelpers.c',
        'vendor\SDL_kitchensink\src\internal\video\kitvideo.c',
        'vendor\SDL_kitchensink\src\internal\video\kitvideoutils.c'
    )
    $cCommon = @(
        '-std=c17',
        '-Wall',
        '-Wextra',
        '-Wpedantic',
        '-DBFPLAYER_PS5=1',
        '-DKIT_VERSION_MAJOR=2',
        '-DKIT_VERSION_MINOR=0',
        '-DKIT_VERSION_PATCH=0',
        '-Ivendor\SDL_kitchensink\include',
        "-I$($pacbrewHome)\include",
        "-I$($pacbrewHome)\include\SDL2",
        '-Iinclude',
        '-include',
        $versionHeaderRelative
    )
    if ($Configuration -eq 'Release') {
        $cCommon += @('-O2', '-DNDEBUG')
    } else {
        $cCommon += @('-O0', '-g3')
    }
    foreach ($source in $cSources) {
        $object = 'build\ps5\' + ([IO.Path]::GetFileNameWithoutExtension($source)) + '.o'
        & $linker @cCommon -c $source -o $object
        if ($LASTEXITCODE -ne 0) {
            throw "Compilation failed: $source"
        }
        $objects += $object
    }
    $sdlVideoOverrideObject = 'build\ps5\SDL_ps5video.override.o'
    Copy-Item -LiteralPath $sdlVideoOverrideSource -Destination (
        Join-Path $projectRoot $sdlVideoOverrideObject) -Force
    $objects += $sdlVideoOverrideObject

    $unstrippedElf = Join-Path $buildDir 'bfplayer.unstripped.elf'
    $linkArgs = @(
        '-o', 'build\ps5\bfplayer.unstripped.elf'
    ) + $objects + @(
        "-L$($pacbrewHome)\lib",
        '-Wl,--start-group',
        '-lSDL2_image',
        '-lSDL2_ttf',
        '-lSDL2',
        '-lavfilter',
        '-lavformat',
        '-lavcodec',
        '-lswscale',
        '-lswresample',
        '-lavutil',
        '-lass',
        '-lfontconfig',
        '-lharfbuzz',
        '-lfribidi',
        '-lfreetype',
        '-lpng16',
        '-lbz2',
        '-lz',
        '-llzma',
        '-lssl',
        '-lcrypto',
        '-liconv',
        '-lsamplerate',
        '-lexpat',
        '-ltinyxml2',
        '-lsqlite3',
        '-lc++',
        '-lc++abi',
        '-lunwind',
        '-Wl,--end-group',
        '-lSceSystemService',
        '-lSceUserService',
        '-lScePad',
        '-lSceAudioOut',
        '-lSceVideoOut',
        '-lSceKeyboard',
        '-lSceImeDialog'
    )
    # ps5-c++.cmd forces `-x c++` globally, which makes Clang parse ELF object
    # files as source during the link step. Use the C driver for linking and
    # explicitly include libc++/libc++abi in the static group above.
    & $linker @linkArgs
    if ($LASTEXITCODE -ne 0) {
        throw 'Link failed.'
    }

    $elf = Join-Path $distDir 'bfplayer.elf'
    $strip = (Get-Command llvm-strip -ErrorAction Stop).Source
    & $strip '--strip-all' '-o' $elf $unstrippedElf
    if ($LASTEXITCODE -ne 0) {
        throw 'Strip failed: bfplayer.elf'
    }
    $playerSize = (Get-Item -LiteralPath $elf).Length
    [IO.File]::AppendAllText(
        $versionHeader,
        "#define BFPLAYER_PLAYER_UNCOMPRESSED_SIZE $($playerSize)UL`n")
    $compressedPlayer = Join-Path $buildDir 'bfplayer.elf.gz'
    $inputStream = [IO.File]::OpenRead($elf)
    try {
        $outputStream = [IO.File]::Create($compressedPlayer)
        try {
            $gzipStream = [IO.Compression.GZipStream]::new(
                $outputStream,
                [IO.Compression.CompressionLevel]::Optimal,
                $true)
            try {
                $inputStream.CopyTo($gzipStream)
            }
            finally {
                $gzipStream.Dispose()
            }
        }
        finally {
            $outputStream.Dispose()
        }
    }
    finally {
        $inputStream.Dispose()
    }
    $standaloneCommon = @(
        '-std=c17',
        '-Wall',
        '-Wextra',
        '-Wpedantic',
        '-O2',
        '-DNDEBUG',
        '-Iinclude',
        '-Isrc\launcher',
        '-Isrc\launcher\core',
        "-I$($pacbrewHome)\include",
        '-include',
        $versionHeaderRelative
    )
    $standaloneSources = @(
        'src\launcher\standalone_launcher.c',
        'src\launcher\standalone_route.c',
        'src\launcher\core\pt.c',
        'src\launcher\core\elfldr.c',
        'src\launcher\core\hbldr.c'
    )
    $standaloneObjects = @()
    foreach ($source in $standaloneSources) {
        $objectName = ($source -replace '^src\\launcher\\', '') -replace '[\\/.]', '_'
        $object = "build\ps5\$objectName.o"
        & $linker @standaloneCommon -c $source -o $object
        if ($LASTEXITCODE -ne 0) {
            throw "Compilation failed: $source"
        }
        $standaloneObjects += $object
    }
    $standalone = Join-Path $distDir 'bfplayer-standalone.elf'
    $standaloneLinkArgs = @(
        '-o', 'dist\bfplayer-standalone.elf'
    ) + $standaloneObjects + @(
        '-lkernel_sys',
        '-lSceSystemService',
        '-lSceUserService',
        '-lSceAppInstUtil',
        "-L$($pacbrewHome)\lib",
        '-lz'
    )
    & $linker @standaloneLinkArgs
    if ($LASTEXITCODE -ne 0) {
        throw 'Link failed: bfplayer-standalone.elf'
    }

    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $elf).Hash.ToLowerInvariant()
    $standaloneHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $standalone).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'bfplayer.sha256'),
        "$hash  bfplayer.elf`n")
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'bfplayer-standalone.sha256'),
        "$standaloneHash  bfplayer-standalone.elf`n")

    $expectedPackagePrefix = [IO.Path]::GetFullPath($packageRoot) + [IO.Path]::DirectorySeparatorChar
    $resolvedPackageDir = [IO.Path]::GetFullPath($packageDir)
    if (-not $resolvedPackageDir.StartsWith($expectedPackagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean package staging outside $packageRoot"
    }
    if (Test-Path -LiteralPath $resolvedPackageDir) {
        Remove-Item -LiteralPath $resolvedPackageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
    Copy-Item -LiteralPath $standalone -Destination $packageDir -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $packageDir -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $packageDir -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\STANDALONE_LAUNCHER.md') -Destination (Join-Path $packageDir 'INSTALL.md') -Force

    $payloadFiles = @(
        'bfplayer-standalone.elf',
        'INSTALL.md',
        'LICENSE',
        'THIRD_PARTY_NOTICES.md'
    )
    $fileManifest = @(
        foreach ($relativePath in $payloadFiles) {
            $payloadPath = Join-Path $packageDir $relativePath
            $payloadItem = Get-Item -LiteralPath $payloadPath
            [ordered]@{
                path = $relativePath.Replace('\', '/')
                bytes = $payloadItem.Length
                sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $payloadPath).Hash.ToLowerInvariant()
            }
        }
    )
    $manifest = [ordered]@{
        name = 'BFplayer'
        version = $version
        target = 'x86_64-sie-ps5'
        package = 'single standalone payload'
        direct_tile = 'PSMC00001 via loopback-only localhost:9040/launch'
        websrv_required = $false
        embedded_player = $true
        embedded_player_compression = 'gzip'
        embedded_player_bytes = $playerSize
        embedded_player_compressed_bytes = (Get-Item -LiteralPath $compressedPlayer).Length
        pacbrew = 'v0.37'
        ffmpeg = '7.0.1'
        player_sha256 = $hash
        standalone_sha256 = $standaloneHash
        built_utc = [DateTime]::UtcNow.ToString('o')
        files = $fileManifest
    }
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'build-manifest.json'),
        (($manifest | ConvertTo-Json -Depth 5) + "`n"))
    Copy-Item -LiteralPath (Join-Path $distDir 'build-manifest.json') -Destination $packageDir -Force

    $expectedPackageFiles = @(
        $payloadFiles + @('build-manifest.json') |
            ForEach-Object { $_.Replace('\', '/') } |
            Sort-Object
    )
    $actualPackageFiles = @(
        Get-ChildItem -LiteralPath $packageDir -File -Recurse |
            ForEach-Object {
                $_.FullName.Substring($packageDir.Length + 1).Replace('\', '/')
            } |
            Sort-Object
    )
    $packageDifference = Compare-Object $expectedPackageFiles $actualPackageFiles
    if ($packageDifference) {
        throw "Package staging contains a missing or unexpected file: $($packageDifference | Out-String)"
    }

    $zip = Join-Path $distDir 'BFplayer-standalone.zip'
    Compress-Archive -Path $packageDir -DestinationPath $zip -CompressionLevel Optimal -Force
    $zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'BFplayer-standalone.zip.sha256'),
        "$zipHash  BFplayer-standalone.zip`n")

    $resolvedWebsrvPackageDir = [IO.Path]::GetFullPath($websrvPackageDir)
    if (-not $resolvedWebsrvPackageDir.StartsWith(
            $expectedPackagePrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean websrv package staging outside $packageRoot"
    }
    if (Test-Path -LiteralPath $resolvedWebsrvPackageDir) {
        Remove-Item -LiteralPath $resolvedWebsrvPackageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path `
        $websrvPackageDir, `
        (Join-Path $websrvPackageDir 'assets\fonts'), `
        (Join-Path $websrvPackageDir 'sce_sys') | Out-Null

    $websrvEboot = Join-Path $websrvPackageDir 'eboot.elf'
    Copy-Item -LiteralPath $elf -Destination $websrvEboot -Force
    Copy-Item `
        -LiteralPath (Join-Path $projectRoot 'assets\fonts\NotoSans-Regular.ttf') `
        -Destination (Join-Path $websrvPackageDir 'assets\fonts') `
        -Force
    Copy-Item `
        -LiteralPath (Join-Path $projectRoot 'assets\fonts\OFL.txt') `
        -Destination (Join-Path $websrvPackageDir 'assets\fonts') `
        -Force
    Copy-Item `
        -LiteralPath (Join-Path $projectRoot 'assets\icon0.png') `
        -Destination (Join-Path $websrvPackageDir 'sce_sys\icon0.png') `
        -Force
    Copy-Item `
        -LiteralPath (Join-Path $projectRoot 'docs\WEBSRV.md') `
        -Destination (Join-Path $websrvPackageDir 'INSTALL.md') `
        -Force
    Copy-Item `
        -LiteralPath (Join-Path $projectRoot 'LICENSE') `
        -Destination $websrvPackageDir `
        -Force
    Copy-Item `
        -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') `
        -Destination $websrvPackageDir `
        -Force

    $websrvPayloadFiles = @(
        'eboot.elf',
        'assets\fonts\NotoSans-Regular.ttf',
        'assets\fonts\OFL.txt',
        'sce_sys\icon0.png',
        'INSTALL.md',
        'LICENSE',
        'THIRD_PARTY_NOTICES.md'
    )
    $websrvFileManifest = @(
        foreach ($relativePath in $websrvPayloadFiles) {
            $payloadPath = Join-Path $websrvPackageDir $relativePath
            $payloadItem = Get-Item -LiteralPath $payloadPath
            [ordered]@{
                path = $relativePath.Replace('\', '/')
                bytes = $payloadItem.Length
                sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $payloadPath).Hash.ToLowerInvariant()
            }
        }
    )
    $websrvManifest = [ordered]@{
        name = 'BFplayer'
        version = $version
        target = 'x86_64-sie-ps5'
        package = 'websrv homebrew folder'
        install_directory = 'BFplayer'
        entrypoint = 'BFplayer/eboot.elf'
        websrv_required = $true
        resident_bfplayer_launcher = $false
        runtime_data = '/data/BFplayer'
        pacbrew = 'v0.37'
        ffmpeg = '7.0.1'
        player_sha256 = $hash
        built_utc = $manifest.built_utc
        files = $websrvFileManifest
    }
    [IO.File]::WriteAllText(
        (Join-Path $websrvPackageDir 'build-manifest.json'),
        (($websrvManifest | ConvertTo-Json -Depth 5) + "`n"))

    $expectedWebsrvFiles = @(
        $websrvPayloadFiles + @('build-manifest.json') |
            ForEach-Object { $_.Replace('\', '/') } |
            Sort-Object
    )
    $actualWebsrvFiles = @(
        Get-ChildItem -LiteralPath $websrvPackageDir -File -Recurse |
            ForEach-Object {
                $_.FullName.Substring($websrvPackageDir.Length + 1).Replace('\', '/')
            } |
            Sort-Object
    )
    $websrvDifference = Compare-Object $expectedWebsrvFiles $actualWebsrvFiles
    if ($websrvDifference) {
        throw "Websrv staging contains a missing or unexpected file: $($websrvDifference | Out-String)"
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $websrvEboot).Hash.ToLowerInvariant() -ne $hash) {
        throw 'Websrv eboot.elf does not match the built BFplayer ELF.'
    }

    $elfStream = [IO.File]::OpenRead($websrvEboot)
    try {
        $elfMagic = New-Object byte[] 4
        if ($elfStream.Read($elfMagic, 0, $elfMagic.Length) -ne $elfMagic.Length -or
            $elfMagic[0] -ne 0x7f -or
            $elfMagic[1] -ne 0x45 -or
            $elfMagic[2] -ne 0x4c -or
            $elfMagic[3] -ne 0x46) {
            throw 'Websrv eboot.elf is not an ELF file.'
        }
    }
    finally {
        $elfStream.Dispose()
    }

    $iconPath = Join-Path $websrvPackageDir 'sce_sys\icon0.png'
    $iconStream = [IO.File]::OpenRead($iconPath)
    try {
        $pngMagic = New-Object byte[] 8
        if ($iconStream.Read($pngMagic, 0, $pngMagic.Length) -ne $pngMagic.Length -or
            [BitConverter]::ToString($pngMagic) -ne '89-50-4E-47-0D-0A-1A-0A') {
            throw 'Websrv sce_sys/icon0.png is not a PNG file.'
        }
    }
    finally {
        $iconStream.Dispose()
    }

    $websrvZip = Join-Path $distDir 'BFplayer-websrv.zip'
    Compress-Archive `
        -Path $websrvPackageDir `
        -DestinationPath $websrvZip `
        -CompressionLevel Optimal `
        -Force

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $websrvArchive = [IO.Compression.ZipFile]::OpenRead($websrvZip)
    try {
        $archiveFiles = @(
            $websrvArchive.Entries |
                Where-Object { $_.Name } |
                ForEach-Object { $_.FullName.Replace('\', '/') } |
                Sort-Object
        )
        $expectedArchiveFiles = @(
            $expectedWebsrvFiles |
                ForEach-Object { "BFplayer/$_" } |
                Sort-Object
        )
        $archiveDifference = Compare-Object $expectedArchiveFiles $archiveFiles
        if ($archiveDifference) {
            throw "Websrv ZIP contains a missing or unexpected file: $($archiveDifference | Out-String)"
        }
        if ($archiveFiles -contains 'BFplayer/homebrew.js') {
            throw 'Websrv ZIP must launch eboot.elf directly, not through homebrew.js.'
        }
    }
    finally {
        $websrvArchive.Dispose()
    }

    $resolvedWebsrvVerifyDir = [IO.Path]::GetFullPath($websrvVerifyDir)
    if (-not $resolvedWebsrvVerifyDir.StartsWith(
            $expectedPackagePrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean websrv verification staging outside $packageRoot"
    }
    if (Test-Path -LiteralPath $resolvedWebsrvVerifyDir) {
        Remove-Item -LiteralPath $resolvedWebsrvVerifyDir -Recurse -Force
    }
    [IO.Compression.ZipFile]::ExtractToDirectory($websrvZip, $websrvVerifyDir)
    foreach ($relativePath in $expectedWebsrvFiles) {
        $stagedPath = Join-Path $websrvPackageDir $relativePath
        $extractedPath = Join-Path (Join-Path $websrvVerifyDir 'BFplayer') $relativePath
        if (-not (Test-Path -LiteralPath $extractedPath -PathType Leaf)) {
            throw "Websrv ZIP verification is missing $relativePath"
        }
        $stagedHash =
            (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedPath).Hash
        $extractedHash =
            (Get-FileHash -Algorithm SHA256 -LiteralPath $extractedPath).Hash
        if ($stagedHash -ne $extractedHash) {
            throw "Websrv ZIP verification hash mismatch: $relativePath"
        }
    }
    Remove-Item -LiteralPath $resolvedWebsrvVerifyDir -Recurse -Force

    $websrvZipHash =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $websrvZip).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'BFplayer-websrv.zip.sha256'),
        "$websrvZipHash  BFplayer-websrv.zip`n")

    $logDir = Join-Path $workspaceRoot 'logs\build'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    [IO.File]::WriteAllLines(
        (Join-Path $logDir 'bfplayer-latest.log'),
        @(
            "name=BFplayer",
            "version=$version",
            "configuration=$Configuration",
            "pacbrew=$pacbrewHome",
            "elf=$elf",
            "elf_bytes=$((Get-Item -LiteralPath $elf).Length)",
            "elf_compressed_bytes=$((Get-Item -LiteralPath $compressedPlayer).Length)",
            "elf_sha256=$hash",
            "standalone=$standalone",
            "standalone_bytes=$((Get-Item -LiteralPath $standalone).Length)",
            "standalone_sha256=$standaloneHash",
            "package=$zip",
            "package_sha256=$zipHash",
            "websrv_package=$websrvZip",
            "websrv_package_sha256=$websrvZipHash",
            "built_utc=$($manifest.built_utc)"
        ))
    Get-Item -LiteralPath $standalone | Select-Object FullName, Length
    Get-Item -LiteralPath $websrvZip | Select-Object FullName, Length
    Write-Host "SHA256 $standaloneHash"
    Write-Host "WEBSRV SHA256 $websrvZipHash"
}
finally {
    Pop-Location
}
