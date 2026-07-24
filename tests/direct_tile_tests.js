'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const param = JSON.parse(
    fs.readFileSync(path.join(root, 'assets', 'tile', 'param.json'), 'utf8'));

assert.strictEqual(param.applicationCategoryType, 65536);
assert.strictEqual(param.titleId, 'PSMC00001');
assert.deepStrictEqual(Object.keys(param.localizedParameters).sort(), [
    'defaultLanguage',
    'en-US'
]);
assert.strictEqual(param.localizedParameters.defaultLanguage, 'en-US');
assert.strictEqual(
    param.localizedParameters['en-US'].titleName,
    'PS5 Media Center');

const uri = new URL(param.deeplinkUri);
assert.strictEqual(uri.protocol, 'http:');
assert.strictEqual(uri.hostname, '127.0.0.1');
assert.strictEqual(uri.port, '8080');
assert.strictEqual(uri.pathname, '/hbldr');
assert.strictEqual(
    uri.searchParams.get('path'),
    '/data/homebrew/PS5-MediaCenter/ps5-media-center.elf');
assert.strictEqual(
    uri.searchParams.get('cwd'),
    '/data/homebrew/PS5-MediaCenter');
assert.strictEqual(uri.searchParams.get('pipe'), '0');
assert.strictEqual(uri.username, '');
assert.strictEqual(uri.password, '');

const installer = fs.readFileSync(
    path.join(root, 'src', 'launcher', 'tile_installer.c'),
    'utf8');
assert(installer.includes('PS5MC_APPINST_AUTHID'));
assert(installer.includes('AppInstallTitleDir'));
assert(installer.includes('assets/tile/param.json'));
assert(installer.includes('assets/icon0.png'));
assert(!installer.includes('unlink('));

console.log('direct_tile_tests: PASS');
