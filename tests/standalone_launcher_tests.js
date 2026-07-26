'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const param = JSON.parse(
    fs.readFileSync(path.join(root, 'assets', 'tile', 'param.json'), 'utf8'));
const launcher = fs.readFileSync(
    path.join(root, 'src', 'launcher', 'standalone_launcher.c'),
    'utf8');
const hbldr = fs.readFileSync(
    path.join(root, 'src', 'launcher', 'core', 'hbldr.c'),
    'utf8');
const ptraceCore = fs.readFileSync(
    path.join(root, 'src', 'launcher', 'core', 'pt.c'),
    'utf8');
const libraryUi = fs.readFileSync(
    path.join(root, 'src', 'ui', 'library_ui.cpp'),
    'utf8');
const player = fs.readFileSync(
    path.join(root, 'src', 'main.cpp'),
    'utf8');
const playbackOsd = fs.readFileSync(
    path.join(root, 'src', 'ui', 'playback_osd.cpp'),
    'utf8');
const build = fs.readFileSync(path.join(root, 'build.ps1'), 'utf8');

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
assert.strictEqual(uri.port, '9040');
assert.strictEqual(uri.pathname, '/launch');
assert.strictEqual(uri.search, '');
assert.strictEqual(uri.username, '');
assert.strictEqual(uri.password, '');
assert.notStrictEqual(uri.port, '8080');

assert(launcher.includes('#define PS5MC_SERVICE_PORT 9040'));
assert(launcher.includes('htonl(INADDR_LOOPBACK)'));
assert(launcher.includes('ps5mc_request_is_launch'));
assert(launcher.includes('ps5mc_request_is_shutdown'));
assert(launcher.includes('create_loopback_listener_with_takeover'));
assert(launcher.includes('acquire_instance_lock'));
assert(launcher.includes('flock(descriptor, LOCK_EX | LOCK_NB)'));
assert(launcher.includes('PS5MC_INSTANCE_LOCK_PATH'));
assert(launcher.includes('serialized takeover lock acquired'));
assert(launcher.includes('regular_file_matches'));
assert(launcher.includes('update_file_atomic'));
assert(launcher.includes('runtime assets version=%s changed_files=%d'));
assert(launcher.includes('request route=/shutdown action=exit'));
assert(launcher.includes('hbldr_launch_buffer'));
assert(launcher.includes('build/ps5/ps5-media-center.elf.gz'));
assert(launcher.includes('inflateInit2'));
assert(launcher.includes('PS5MC_PLAYER_UNCOMPRESSED_SIZE'));
assert(launcher.includes('assets/fonts/NotoSans-Regular.ttf'));
assert(launcher.includes('assets/tile/param.json'));
assert(launcher.includes('assets/icon0.png'));
assert(launcher.includes('remove_legacy_bigapp_host_registration'));
assert(launcher.includes('PS5MC_LEGACY_CLEAN_MARKER'));
assert(launcher.includes('legacy host cleanup already complete; skipping'));
assert(launcher.includes('sceAppInstUtilAppUnInstall'));
assert(launcher.includes('hbldr_prepare_host'));
assert(launcher.includes('PSMR00001'));
assert(!launcher.includes('PS5MC_HOST_APP_DIR'));
assert(!launcher.includes('install_bigapp_host_registration'));
assert(launcher.includes('install_title_dir'));
assert(launcher.includes('Wudg3Xe3heE'));
assert(launcher.includes('Content-Type: %s; charset=utf-8'));
assert(launcher.includes('Launching PS5 Media Center'));
assert(launcher.includes('window.close()'));
assert(launcher.includes('self.close()'));
assert(launcher.includes('history.go(-1)'));
assert(launcher.includes('press O to close it'));
assert(!launcher.includes('press the PS button'));
assert(launcher.includes('sceKernelSendNotificationRequest'));
assert(launcher.includes('Open it from Media'));
assert(launcher.includes('history.back()'));
assert(!launcher.includes('microhttpd'));
assert(!launcher.includes('#include "websrv'));
assert(!launcher.includes('8080'));
const launcherMain = launcher.slice(launcher.indexOf('int main(void)'));
assert(
    launcherMain.indexOf('acquire_instance_lock()') <
        launcherMain.indexOf('install_runtime_assets()'),
    'reinjection must own the singleton lock before updating runtime files');
assert(
    launcherMain.indexOf('create_loopback_listener_with_takeover()') <
        launcherMain.indexOf('install_runtime_assets()'),
    'reinjection must complete resident-service handover before updating');
assert(
    launcherMain.indexOf('loopback handover complete; applying serialized update') <
        launcherMain.indexOf('install_runtime_assets()'),
    'serialized update begins only after the prior listener is gone');
assert(
    launcherMain.lastIndexOf('create_loopback_listener()') >
        launcherMain.indexOf('install_dashboard_tile()'),
    'final service listener must bind only after the update completes');

assert(hbldr.includes('hbldr_launch_buffer'));
assert(hbldr.includes('hbldr_prepare_host'));
assert(hbldr.includes('#define HOST_TITLE_ID "PSMC00001"'));
assert(hbldr.includes('"  \\"applicationCategoryType\\": 65536,\\n"'));
assert(!hbldr.includes('"  \\"applicationCategoryType\\": 0,\\n"'));
assert(!hbldr.includes('PSMR00001'));
assert(!hbldr.includes('FAKE00000'));
assert(!hbldr.includes('#include "websrv'));
assert(!hbldr.includes('#include "sys.h"'));
const browserStart = libraryUi.indexOf('bool load_browser_directory(');
const browserEnd = libraryUi.indexOf('void report_browser_failure()', browserStart);
assert(browserStart >= 0 && browserEnd > browserStart);
const browserSource = libraryUi.slice(browserStart, browserEnd);
assert(browserSource.includes('AT_SYMLINK_NOFOLLOW'));
assert(!browserSource.includes('if (status.st_dev != root_status.st_dev)'),
    'interactive root browser must show explicit mount points');
assert(libraryUi.includes('const std::string initial = "/"'));
assert(!libraryUi.includes('default_roots()'));
assert(libraryUi.includes('ADD MEDIA FAILED'));
assert(libraryUi.includes('LibraryOverlay::controls'));
assert(libraryUi.includes('LibraryOverlay::settings'));
assert(libraryUi.includes('Import Selected Folder'));
assert(libraryUi.includes('notice_label'));
assert(libraryUi.includes('"Search library"'));
assert(libraryUi.includes('"Clear search"'));
assert(libraryUi.includes('"Cross      Open folder / add movie"'));
assert(libraryUi.includes('"Square     Import as mixed library"'));
assert(libraryUi.includes('"Saves only after import completes"'));
assert(libraryUi.includes('"R3"'));
assert(libraryUi.includes('"Sort"'));
assert(!libraryUi.includes('std::string("Sort: ")'));
assert(libraryUi.includes('library_item_name_less'));
assert(libraryUi.includes('previous_series'));
assert(libraryUi.includes('const bool empty = impl_->entries.empty();'));
assert(!libraryUi.includes(
    'const bool empty = impl_->entries.empty() && !impl_->scanning;'));
assert(libraryUi.includes(
    '"Square       Add Media: Import folder             Audio track"'));
assert(libraryUi.includes('if (browsing && event.type == SDL_KEYDOWN)'));
assert(libraryUi.includes(
    '"Finish or cancel the current library import first"'));
assert(libraryUi.includes('SDL_CreateThread('));
assert(libraryUi.includes('"ps5mc-bulk-import"'));
assert(libraryUi.includes('"Cancel Import"'));
assert(libraryUi.includes('consume_bulk_import_result()'));
const bulkImport = fs.readFileSync(
    path.join(root, 'src', 'core', 'bulk_import.cpp'),
    'utf8');
assert(bulkImport.includes('lstat(path.c_str(), &status)'),
    'PS5 removable-storage bulk import must retain its lstat fallback');
assert(player.includes('PlaybackOverlay::menu'));
assert(player.includes('PlaybackOverlay::controls'));
assert(player.includes('PlaybackOverlay::settings'));
assert(player.includes('persist_active_player_settings'));
assert(player.includes('void toggle_mute(App& app)'));
assert(player.includes('"Unmute audio" : "Mute audio"'));
assert(player.includes('kSettingShortSeekSeconds'));
assert(player.includes('app.settings.short_seek_seconds'));
assert(player.includes('app.settings.long_seek_seconds'));
assert(player.includes('sceSystemServiceGetAppIdOfRunningBigApp'));
assert(player.includes('sceSystemServiceGetAppTitleId'));
assert(player.includes('sceSystemServiceKillApp(app_id, -1, 0, 0)'));
assert(player.includes('return_to_playstation_home();'));
assert(player.includes('kMediaCenterTitleId = "PSMC00001"'));
assert(player.includes('reason=unexpected-bigapp'));
assert(!player.includes('subtitle_delay_mode'),
    'D-pad seeking must not be captured by a hidden subtitle-delay mode');
assert(playbackOsd.includes('maximum_message_width'));
assert(playbackOsd.includes('fit_text_to_width('));
assert(ptraceCore.includes('snprintf(buf, sizeof(buf)'));
assert(!ptraceCore.includes('strcpy(buf, s)'));
assert(build.includes('ps5-media-center-standalone.elf'));
assert(build.includes("'--strip-all'"));
assert(build.includes('GZipStream'));
assert(!build.includes(
    "$zip = Join-Path $distDir 'PS5-MediaCenter-websrv.zip'"));
assert(!build.includes(
    "$tileInstaller = Join-Path $distDir 'ps5mc-tile-installer.elf'"));

console.log('standalone_launcher_tests: PASS');
