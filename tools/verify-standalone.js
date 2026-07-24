'use strict';

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const root = path.join(__dirname, '..');
const dist = path.join(root, 'dist');
const standalone = fs.readFileSync(
    path.join(dist, 'ps5-media-center-standalone.elf'));
const player = fs.readFileSync(path.join(dist, 'ps5-media-center.elf'));
const compressedPlayer = fs.readFileSync(
    path.join(root, 'build', 'ps5', 'ps5-media-center.elf.gz'));
const assets = [
    ['compressed player ELF', compressedPlayer],
    ['tile param', fs.readFileSync(path.join(root, 'assets', 'tile', 'param.json'))],
    ['host param', fs.readFileSync(path.join(root, 'assets', 'fakeapp', 'param.json'))],
    ['icon', fs.readFileSync(path.join(root, 'assets', 'icon0.png'))],
    ['font', fs.readFileSync(
        path.join(root, 'assets', 'fonts', 'NotoSans-Regular.ttf'))],
    ['font license', fs.readFileSync(path.join(root, 'assets', 'fonts', 'OFL.txt'))]
];

assert.strictEqual(standalone.subarray(0, 4).toString('hex'), '7f454c46');
assert.strictEqual(standalone[4], 2, 'standalone must be ELF64');
assert.strictEqual(standalone[7], 9, 'standalone OS/ABI must be FreeBSD');
assert.strictEqual(standalone.readUInt16LE(16), 3, 'standalone must be ET_DYN');
assert.strictEqual(player.subarray(0, 4).toString('hex'), '7f454c46');
assert(zlib.gunzipSync(compressedPlayer).equals(player),
    'embedded gzip must expand to the exact stripped player ELF');
assert.strictEqual(
    standalone.indexOf(player),
    -1,
    'uncompressed player ELF must not also be embedded');

for (const [label, bytes] of assets) {
    const first = standalone.indexOf(bytes);
    assert(first >= 0, `${label} is not embedded`);
    assert.strictEqual(
        standalone.indexOf(bytes, first + 1),
        -1,
        `${label} must be embedded exactly once`);
    assert(standalone.subarray(first, first + bytes.length).equals(bytes));
}

const strings = standalone.toString('latin1');
assert(!strings.includes('127.0.0.1:8080'));
assert(!strings.includes('/hbldr?'));
assert(!strings.includes('libmicrohttpd'));

const sha256 = crypto
    .createHash('sha256')
    .update(standalone)
    .digest('hex');
console.log(`verify-standalone: PASS bytes=${standalone.length} sha256=${sha256}`);
