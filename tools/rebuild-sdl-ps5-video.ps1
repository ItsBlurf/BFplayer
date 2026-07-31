[CmdletBinding()]
param(
    [string]$SourceDirectory = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$workspaceRoot = (Resolve-Path (Join-Path $projectRoot '..\..')).Path
. (Join-Path $workspaceRoot 'ps5dev-env.ps1')

$expectedCommit = '0baf4ac49382b537ba449901b5b6d0d189bb1fbb'
$pacbrewHome = $env:PS5_PACBREW_HOME
if (-not $pacbrewHome) {
    $toolchains = Split-Path $env:PS5_PAYLOAD_SDK -Parent
    $pacbrewHome = Join-Path $toolchains 'pacbrew-v0.37\homebrew'
}
$pacbrewHome = (Resolve-Path -LiteralPath $pacbrewHome).Path

if (-not $SourceDirectory) {
    $SourceDirectory = Join-Path $workspaceRoot 'build\bfplayer-hwdecode-analysis\SDL'
}
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path

$actualCommit = (& git -C $SourceDirectory rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $expectedCommit) {
    throw "SDL source must be exactly $expectedCommit; found $actualCommit"
}
if (& git -C $SourceDirectory status --porcelain) {
    throw "SDL source checkout must be clean: $SourceDirectory"
}

$scratchRoot = Join-Path $projectRoot 'build\sdl-ps5-video-override'
$scratchRoot = [IO.Path]::GetFullPath($scratchRoot)
$projectPrefix = [IO.Path]::GetFullPath($projectRoot) +
    [IO.Path]::DirectorySeparatorChar
if (-not $scratchRoot.StartsWith(
        $projectPrefix,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace scratch directory outside $projectRoot"
}
if (Test-Path -LiteralPath $scratchRoot) {
    Remove-Item -LiteralPath $scratchRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $scratchRoot | Out-Null
Copy-Item -LiteralPath $SourceDirectory -Destination (Join-Path $scratchRoot 'SDL') -Recurse
$scratchSource = Join-Path $scratchRoot 'SDL'

$patch = Join-Path $projectRoot 'vendor\SDL_ps5_backend\ps5-4k-output.patch'
& git -C $scratchSource apply --unidiff-zero --check $patch
if ($LASTEXITCODE -ne 0) {
    throw 'The BFplayer PS5 SDL patch no longer applies cleanly.'
}
& git -C $scratchSource apply --unidiff-zero $patch
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to apply the BFplayer PS5 SDL patch.'
}

$outputDirectory = Join-Path $projectRoot 'vendor\SDL_ps5_backend\lib'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$output = Join-Path $outputDirectory 'SDL_ps5video.c.o'
$compiler = Join-Path $workspaceRoot 'tools\bin\ps5-clang.cmd'
$relativeObject = 'build\SDL_ps5video.c.o'
New-Item -ItemType Directory -Force -Path (
    Join-Path $scratchSource 'build') | Out-Null

Push-Location $scratchSource
try {
    & $compiler `
        '-std=c17' `
        '-O3' `
        '-DNDEBUG' `
        '-fPIC' `
        "-I$($pacbrewHome)\include" `
        "-I$($pacbrewHome)\include\SDL2" `
        '-Iinclude' `
        '-c' `
        'src\video\ps5\SDL_ps5video.c' `
        '-o' `
        $relativeObject
    if ($LASTEXITCODE -ne 0) {
        throw 'Compilation failed for the BFplayer PS5 SDL video override.'
    }
} finally {
    Pop-Location
}

Copy-Item -LiteralPath (
    Join-Path $scratchSource $relativeObject) -Destination $output -Force
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash.ToLowerInvariant()
Write-Host "Built $output"
Write-Host "SHA256 $hash"
