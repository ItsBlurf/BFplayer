const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const alerts = [];
const source = fs.readFileSync(path.join(__dirname, '..', 'homebrew.js'), 'utf8');
const context = {
    URL,
    alert: message => alerts.push(message),
    prompt: () => null,
    pickFile: async () => '/mnt/usb0/Movie.mkv',
    pickDirectory: async () => '/mnt/usb0/Series',
    window: { workingDir: '/data/homebrew/PS5-MediaCenter' }
};
vm.createContext(context);
vm.runInContext(source, context, { filename: 'homebrew.js' });

assert.strictEqual(
    context.validateDirectStreamUrl('  https://media.test/live.m3u8  '),
    'https://media.test/live.m3u8');
assert.strictEqual(
    context.validateDirectStreamUrl('rtsp://192.0.2.10:8554/movie'),
    'rtsp://192.0.2.10:8554/movie');
assert.strictEqual(
    context.validateDirectStreamUrl('udp://239.1.2.3:5000'),
    'udp://239.1.2.3:5000');
assert.strictEqual(context.validateDirectStreamUrl('file:///data/movie.mkv'), null);
assert.strictEqual(context.validateDirectStreamUrl('smb://server/share/movie.mkv'), null);
assert.strictEqual(
    context.validateDirectStreamUrl('https://user:password@media.test/movie.mkv'),
    null);
assert.strictEqual(context.validateDirectStreamUrl('not a URL'), null);
assert.ok(alerts.length >= 4, 'invalid input should explain why it was rejected');

(async () => {
    const extension = await context.main();
    const movieOption = extension.options.find(
        option => option.text === 'Add a movie file to the library');
    const showOption = extension.options.find(
        option => option.text === 'Add a TV show folder to the library');
    const directOption = extension.options.find(
        option => option.text === 'Play a file directly');
    assert.ok(movieOption && showOption && directOption, 'media source options exist');
    assert.deepStrictEqual(
        Array.from((await movieOption.onclick()).args),
        ['--add-movie', '/mnt/usb0/Movie.mkv']);
    assert.deepStrictEqual(
        Array.from((await showOption.onclick()).args),
        ['--add-tv-folder', '/mnt/usb0/Series']);
    assert.deepStrictEqual(
        Array.from((await directOption.onclick()).args),
        ['/mnt/usb0/Movie.mkv']);
    console.log('homebrew_launcher_tests: PASS');
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
