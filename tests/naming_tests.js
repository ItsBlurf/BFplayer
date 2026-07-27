const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const root = path.resolve(__dirname, '..');
const thisFile = path.resolve(__filename);
const oldPrefix = ['ps5', 'mc'].join('');
const oldWords = ['media', 'center'].join('[ -]?');
const forbiddenText = new RegExp(`${oldPrefix}|${oldWords}`, 'i');
const forbiddenPath = new RegExp(`${oldPrefix}|media[ ._-]?center`, 'i');
const textExtensions = new Set([
    '.c', '.cc', '.cpp', '.h', '.hpp', '.js', '.json', '.md',
    '.ps1', '.txt', '.yml', '.yaml',
]);

const failures = [];

const trackedFiles = execFileSync(
    'git',
    ['-C', root, 'ls-files', '-z'],
    { encoding: 'utf8' },
).split('\0').filter(Boolean);

for (const relative of trackedFiles) {
    if (forbiddenPath.test(relative)) {
        failures.push(`old identity remains in path: ${relative}`);
    }
    const fullPath = path.join(root, relative);
    if (
        path.resolve(fullPath) === thisFile ||
        !textExtensions.has(path.extname(relative).toLowerCase())
    ) {
        continue;
    }
    const text = fs.readFileSync(fullPath, 'utf8');
    if (forbiddenText.test(text)) {
        failures.push(`old identity remains in text: ${relative}`);
    }
}

if (failures.length > 0) {
    throw new Error(failures.join('\n'));
}

console.log('naming_tests: PASS');
