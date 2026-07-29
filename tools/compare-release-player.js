'use strict';

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const zlib = require('zlib');

if (process.argv.length !== 4) {
    console.error(
        'Usage: node tools/compare-release-player.js ' +
        '<standalone.elf> <websrv-eboot.elf>',
    );
    process.exit(2);
}

const standalone = fs.readFileSync(process.argv[2]);
const websrvPlayer = fs.readFileSync(process.argv[3]);
const gzipMagic = Buffer.from([0x1f, 0x8b, 0x08]);
const expectedSizeTrailer = Buffer.alloc(4);
expectedSizeTrailer.writeUInt32LE(websrvPlayer.length);

let embeddedPlayer = null;
let gzipOffset = standalone.indexOf(gzipMagic);
while (gzipOffset >= 0 && embeddedPlayer === null) {
    let trailerOffset = standalone.indexOf(
        expectedSizeTrailer,
        gzipOffset + gzipMagic.length,
    );
    while (trailerOffset >= 0) {
        try {
            const candidate = zlib.gunzipSync(
                standalone.subarray(gzipOffset, trailerOffset + 4),
            );
            if (candidate.length === websrvPlayer.length) {
                embeddedPlayer = candidate;
                break;
            }
        } catch {
            // This size pattern belonged to compressed data, not the trailer.
        }
        trailerOffset = standalone.indexOf(
            expectedSizeTrailer,
            trailerOffset + 1,
        );
    }
    gzipOffset = standalone.indexOf(gzipMagic, gzipOffset + 1);
}

assert(embeddedPlayer, 'embedded player gzip was not found');

function sha256(bytes) {
    return crypto.createHash('sha256').update(bytes).digest('hex');
}

const result = {
    standalone_bytes: standalone.length,
    embedded_player_bytes: embeddedPlayer.length,
    embedded_player_sha256: sha256(embeddedPlayer),
    websrv_eboot_bytes: websrvPlayer.length,
    websrv_eboot_sha256: sha256(websrvPlayer),
    byte_identical: embeddedPlayer.equals(websrvPlayer),
};

console.log(JSON.stringify(result, null, 2));
assert(result.byte_identical, 'released standalone and websrv players differ');
console.log('compare-release-player: PASS');
