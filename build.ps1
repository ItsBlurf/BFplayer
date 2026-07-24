[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
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

$required = @(
    'include\SDL2\SDL.h',
    'include\kitchensink2\kitchensink.h',
    'include\SDL2\SDL_image.h',
    'lib\libSDL2.a',
    'lib\libSDL2_image.a',
    'lib\libSDL_kitchensink.a',
    'lib\libavformat.a',
    'lib\libass.a'
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
$packageDir = Join-Path $packageRoot 'PS5-MediaCenter'
New-Item -ItemType Directory -Force -Path $buildDir, $distDir | Out-Null

$common = @(
    '-std=c++20',
    '-Wall',
    '-Wextra',
    '-Wpedantic',
    '-DPS5MC_PS5=1',
    "-I$($pacbrewHome)\include",
    "-I$($pacbrewHome)\include\SDL2",
    '-Iinclude'
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
        'src\core\diagnostics.cpp',
        'src\core\library_scanner.cpp',
        'src\core\library_view.cpp',
        'src\core\media_sources.cpp',
        'src\core\playlist.cpp',
        'src\core\safe_read_file.cpp',
        'src\core\source_uri.cpp',
        'src\core\video_layout.cpp',
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
        'src\playback\kitsubimage_safe.c'
    )
    $cCommon = @(
        '-std=c17',
        '-Wall',
        '-Wextra',
        '-Wpedantic',
        '-DPS5MC_PS5=1',
        "-I$($pacbrewHome)\include",
        "-I$($pacbrewHome)\include\SDL2",
        '-Iinclude'
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

    $linkArgs = @(
        '-o', 'dist\ps5-media-center.elf'
    ) + $objects + @(
        "-L$($pacbrewHome)\lib",
        '-Wl,--start-group',
        '-lSDL_kitchensink',
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

    $elf = Join-Path $distDir 'ps5-media-center.elf'
    $tileInstallerObject = 'build\ps5\tile_installer.o'
    $tileInstallerCommon = @(
        '-std=c17',
        '-Wall',
        '-Wextra',
        '-Wpedantic',
        '-O2',
        '-DNDEBUG',
        '-Iinclude'
    )
    & $linker @tileInstallerCommon -c 'src\launcher\tile_installer.c' -o $tileInstallerObject
    if ($LASTEXITCODE -ne 0) {
        throw 'Compilation failed: src\launcher\tile_installer.c'
    }
    $tileInstaller = Join-Path $distDir 'ps5mc-tile-installer.elf'
    $tileLinkArgs = @(
        '-o', 'dist\ps5mc-tile-installer.elf',
        $tileInstallerObject,
        '-lkernel_sys',
        '-lSceSystemService',
        '-lSceUserService',
        '-lSceAppInstUtil'
    )
    & $linker @tileLinkArgs
    if ($LASTEXITCODE -ne 0) {
        throw 'Link failed: ps5mc-tile-installer.elf'
    }

    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $elf).Hash.ToLowerInvariant()
    $tileHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $tileInstaller).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'ps5-media-center.sha256'),
        "$hash  ps5-media-center.elf`n")
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'ps5mc-tile-installer.sha256'),
        "$tileHash  ps5mc-tile-installer.elf`n")
    $fontDist = Join-Path $distDir 'assets\fonts'
    New-Item -ItemType Directory -Force -Path $fontDist | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'assets\fonts\NotoSans-Regular.ttf') -Destination $fontDist -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'assets\fonts\OFL.txt') -Destination $fontDist -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'homebrew.js') -Destination $distDir -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $distDir -Force
    $iconDist = Join-Path $distDir 'sce_sys'
    New-Item -ItemType Directory -Force -Path $iconDist | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'assets\icon0.png') -Destination $iconDist -Force

    $expectedPackagePrefix = [IO.Path]::GetFullPath($packageRoot) + [IO.Path]::DirectorySeparatorChar
    $resolvedPackageDir = [IO.Path]::GetFullPath($packageDir)
    if (-not $resolvedPackageDir.StartsWith($expectedPackagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean package staging outside $packageRoot"
    }
    if (Test-Path -LiteralPath $resolvedPackageDir) {
        Remove-Item -LiteralPath $resolvedPackageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $packageDir 'assets\fonts'), (Join-Path $packageDir 'sce_sys') | Out-Null
    Copy-Item -LiteralPath $elf -Destination $packageDir -Force
    Copy-Item -LiteralPath (Join-Path $distDir 'homebrew.js') -Destination $packageDir -Force
    Copy-Item -LiteralPath (Join-Path $distDir 'THIRD_PARTY_NOTICES.md') -Destination $packageDir -Force
    Copy-Item -LiteralPath (Join-Path $fontDist 'NotoSans-Regular.ttf') -Destination (Join-Path $packageDir 'assets\fonts') -Force
    Copy-Item -LiteralPath (Join-Path $fontDist 'OFL.txt') -Destination (Join-Path $packageDir 'assets\fonts') -Force
    Copy-Item -LiteralPath (Join-Path $iconDist 'icon0.png') -Destination (Join-Path $packageDir 'sce_sys') -Force

    $payloadFiles = @(
        'ps5-media-center.elf',
        'homebrew.js',
        'THIRD_PARTY_NOTICES.md',
        'assets\fonts\NotoSans-Regular.ttf',
        'assets\fonts\OFL.txt',
        'sce_sys\icon0.png'
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
    $version = (Get-Content -Raw (Join-Path $projectRoot 'VERSION')).Trim()
    $manifest = [ordered]@{
        name = 'PS5 Media Center'
        version = $version
        target = 'x86_64-sie-ps5'
        package = 'direct dashboard tile + websrv BigApp transition'
        direct_tile = 'PSMC00001 via localhost:8080/hbldr'
        pacbrew = 'v0.37'
        ffmpeg = '7.0.1'
        sha256 = $hash
        tile_installer_sha256 = $tileHash
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

    $zip = Join-Path $distDir 'PS5-MediaCenter-websrv.zip'
    Compress-Archive -Path $packageDir -DestinationPath $zip -CompressionLevel Optimal -Force
    $zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'PS5-MediaCenter-websrv.zip.sha256'),
        "$zipHash  PS5-MediaCenter-websrv.zip`n")

    $directStage = Join-Path $packageRoot 'PS5-MediaCenter-direct-tile'
    $expectedDirectPrefix = [IO.Path]::GetFullPath($packageRoot) + [IO.Path]::DirectorySeparatorChar
    $resolvedDirectStage = [IO.Path]::GetFullPath($directStage)
    if (-not $resolvedDirectStage.StartsWith($expectedDirectPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean direct-tile staging outside $packageRoot"
    }
    if (Test-Path -LiteralPath $resolvedDirectStage) {
        Remove-Item -LiteralPath $resolvedDirectStage -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $resolvedDirectStage | Out-Null
    Copy-Item -LiteralPath $packageDir -Destination $resolvedDirectStage -Recurse -Force
    Copy-Item -LiteralPath $tileInstaller -Destination $resolvedDirectStage -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\DIRECT_TILE.md') -Destination (Join-Path $resolvedDirectStage 'INSTALL-DIRECT-TILE.md') -Force
    $directZip = Join-Path $distDir 'PS5-MediaCenter-direct-tile.zip'
    Compress-Archive -Path (Join-Path $resolvedDirectStage '*') -DestinationPath $directZip -CompressionLevel Optimal -Force
    $directZipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $directZip).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText(
        (Join-Path $distDir 'PS5-MediaCenter-direct-tile.zip.sha256'),
        "$directZipHash  PS5-MediaCenter-direct-tile.zip`n")

    $logDir = Join-Path $workspaceRoot 'logs\build'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    [IO.File]::WriteAllLines(
        (Join-Path $logDir 'ps5-media-center-latest.log'),
        @(
            "name=PS5 Media Center",
            "version=$version",
            "configuration=$Configuration",
            "pacbrew=$pacbrewHome",
            "elf=$elf",
            "elf_bytes=$((Get-Item -LiteralPath $elf).Length)",
            "elf_sha256=$hash",
            "package=$zip",
            "package_sha256=$zipHash",
            "tile_installer=$tileInstaller",
            "tile_installer_sha256=$tileHash",
            "direct_tile_package=$directZip",
            "direct_tile_package_sha256=$directZipHash",
            "built_utc=$($manifest.built_utc)"
        ))
    Get-Item -LiteralPath $elf | Select-Object FullName, Length
    Write-Host "SHA256 $hash"
}
finally {
    Pop-Location
}
