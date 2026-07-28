const assert = require('assert');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const build = fs.readFileSync(path.join(root, 'build.ps1'), 'utf8');
const player = fs.readFileSync(path.join(root, 'src', 'main.cpp'), 'utf8');
const readme = fs.readFileSync(path.join(root, 'README.md'), 'utf8');
const install = fs.readFileSync(path.join(root, 'docs', 'WEBSRV.md'), 'utf8');
const verifier = fs.readFileSync(
    path.join(root, 'tools', 'verify-websrv-package.ps1'),
    'utf8',
);

assert(build.includes("$websrvPackageDir = Join-Path $packageRoot 'BFplayer'"));
assert(build.includes("$websrvEboot = Join-Path $websrvPackageDir 'eboot.elf'"));
assert(build.includes("$websrvZip = Join-Path $distDir 'BFplayer-websrv.zip'"));
assert(build.includes("'assets\\fonts\\NotoSans-Regular.ttf'"));
assert(build.includes("'sce_sys\\icon0.png'"));
assert(build.includes("entrypoint = 'BFplayer/eboot.elf'"));
assert(build.includes('websrv_required = $true'));
assert(build.includes('resident_bfplayer_launcher = $false'));
assert(build.includes("[IO.Compression.ZipFile]::OpenRead($websrvZip)"));
assert(build.includes("[IO.Compression.ZipFile]::ExtractToDirectory("));
assert(build.includes("if ($archiveFiles -contains 'BFplayer/homebrew.js')"));
assert(build.includes("'89-50-4E-47-0D-0A-1A-0A'"));
assert(build.includes("'Websrv eboot.elf is not an ELF file.'"));

assert(player.includes('kWebsrvTitleId = "FAKE00000"'));
assert(player.includes('!is_bfplayer_host && !is_websrv_host'));

const readmeTop = readme.split(/\r?\n/).slice(0, 10).join('\n');
assert(readmeTop.includes('developed with OpenAI Codex'));
assert(readme.includes('## Contributors'));
assert(readme.includes('OpenAI Codex'));
assert(readme.includes('## Acknowledgements'));

assert(install.includes('/data/homebrew/BFplayer/eboot.elf'));
assert(install.includes('/mnt/usbN/homebrew/BFplayer/eboot.elf'));
assert(install.includes('/mnt/extN/homebrew/BFplayer/eboot.elf'));
assert(install.includes('does not contain `homebrew.js`'));
assert(install.includes('does not need `bfplayer-standalone.elf`'));

assert(verifier.includes("'BFplayer/eboot.elf'"));
assert(verifier.includes("$header[7] -ne 9"));
assert(verifier.includes("$header[16] -ne 3"));
assert(verifier.includes("$header[18] -ne 0x3e"));
assert(verifier.includes("Result = 'PASS'"));

console.log('websrv_package_tests: PASS');
