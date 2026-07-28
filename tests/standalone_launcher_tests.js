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
const subtitleBrowser = fs.readFileSync(
    path.join(root, 'src', 'core', 'subtitle_browser.cpp'),
    'utf8');
const demuxThread = fs.readFileSync(
    path.join(
        root,
        'vendor',
        'SDL_kitchensink',
        'src',
        'internal',
        'kitdemuxerthread.c'),
    'utf8');
const playbackOsd = fs.readFileSync(
    path.join(root, 'src', 'ui', 'playback_osd.cpp'),
    'utf8');
const diagnostics = fs.readFileSync(
    path.join(root, 'src', 'core', 'diagnostics.cpp'),
    'utf8');
const kitchensinkDecoder = fs.readFileSync(
    path.join(
        root,
        'vendor',
        'SDL_kitchensink',
        'src',
        'internal',
        'kitdecoder.c'),
    'utf8');
const kitchensinkVideoUtils = fs.readFileSync(
    path.join(
        root,
        'vendor',
        'SDL_kitchensink',
        'src',
        'internal',
        'video',
        'kitvideoutils.c'),
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
    'BFplayer');

const uri = new URL(param.deeplinkUri);
assert.strictEqual(uri.protocol, 'http:');
assert.strictEqual(uri.hostname, '127.0.0.1');
assert.strictEqual(uri.port, '9040');
assert.strictEqual(uri.pathname, '/launch');
assert.strictEqual(uri.search, '');
assert.strictEqual(uri.username, '');
assert.strictEqual(uri.password, '');
assert.notStrictEqual(uri.port, '8080');

assert(launcher.includes('#define BFPLAYER_SERVICE_PORT 9040'));
assert(launcher.includes('htonl(INADDR_LOOPBACK)'));
assert(launcher.includes('bfplayer_request_is_launch'));
assert(launcher.includes('bfplayer_request_is_shutdown'));
assert(launcher.includes('create_loopback_listener_with_takeover'));
assert(launcher.includes('acquire_instance_lock'));
assert(launcher.includes('flock(descriptor, LOCK_EX | LOCK_NB)'));
assert(launcher.includes('BFPLAYER_INSTANCE_LOCK_PATH'));
assert(launcher.includes('serialized takeover lock acquired'));
assert(launcher.includes('active_player_pid'));
assert(launcher.includes('reason=player-already-running'));
assert(launcher.includes('kill(previous_pid, 0)'));
assert(launcher.includes('waitpid(previous_pid, NULL, WNOHANG)'));
assert(launcher.includes('regular_file_matches'));
assert(launcher.includes('update_file_atomic'));
assert(launcher.includes('runtime assets version=%s changed_files=%d'));
assert(launcher.includes('request route=/shutdown action=exit'));
assert(launcher.includes('hbldr_launch_buffer'));
assert(launcher.includes('build/ps5/bfplayer.elf.gz'));
assert(launcher.includes('inflateInit2'));
assert(launcher.includes('BFPLAYER_PLAYER_UNCOMPRESSED_SIZE'));
assert(launcher.includes('install_player_image'));
assert(launcher.includes('installed_player_matches'));
assert(launcher.includes('source=stream-install'));
assert(launcher.includes('"crc32-validation"'));
assert(launcher.includes('mode=mmap-installed'));
assert(launcher.includes('launch stage=runtime-ready'));
assert(launcher.includes('launch stage=map-ready'));
assert(launcher.includes('launch stage=hbldr-begin'));
assert(launcher.includes('launch stage=hbldr-return'));
assert(launcher.includes('O_RDONLY | O_NOFOLLOW'));
assert(launcher.includes('MAP_PRIVATE'));
assert(launcher.includes('munmap'));
assert(launcher.includes('assets/fonts/NotoSans-Regular.ttf'));
assert(launcher.includes('assets/tile/param.json'));
assert(launcher.includes('assets/icon0.png'));
assert(launcher.includes('remove_legacy_bigapp_host_registration'));
assert(launcher.includes('BFPLAYER_LEGACY_CLEAN_MARKER'));
assert(launcher.includes('legacy host cleanup already complete; skipping'));
assert(launcher.includes('sceAppInstUtilAppUnInstall'));
assert(launcher.includes('hbldr_prepare_host'));
assert(launcher.includes('PSMR00001'));
assert(!launcher.includes('BFPLAYER_HOST_APP_DIR'));
assert(!launcher.includes('install_bigapp_host_registration'));
assert(launcher.includes('install_title_dir'));
assert(launcher.includes('Wudg3Xe3heE'));
assert(launcher.includes('Content-Type: %s; charset=utf-8'));
assert(launcher.includes('Launching BFplayer'));
assert(launcher.includes('window.close()'));
assert(launcher.includes('self.close()'));
assert(launcher.includes('history.go(-1)'));
assert(launcher.includes('press O to close it'));
assert(launcher.includes('let n=5'));
assert(launcher.includes('elapsed_ms < 5000'));
assert(launcher.includes('reason=duplicate-request'));
assert(!launcher.includes('press the PS button'));
assert(launcher.includes('sceKernelSendNotificationRequest'));
assert(launcher.includes('Open it from Media'));
assert(launcher.includes('history.back()'));
assert(!launcher.includes('microhttpd'));
assert(!launcher.includes('#include "websrv'));
assert(!launcher.includes('8080'));
const playerLaunch = launcher.slice(
    launcher.indexOf('static int launch_installed_player(void)'),
    launcher.indexOf('static void serve_forever'));
assert(playerLaunch.includes('mmap('));
assert(!playerLaunch.includes('malloc('));
assert(!playerLaunch.includes('inflate('));
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
assert(hbldr.includes('BFPLAYER00000000'));
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
assert(libraryUi.includes('FooterGlyph::triangle, "", "Remove"'));
assert(libraryUi.includes('draw_logo(impl_->renderer, impl_->logo_texture'));
assert(libraryUi.includes('title = "BFplayer"'));
assert(libraryUi.includes('"Exit BFplayer"'));
const legacyVisibleName = `Media ${'Center'}`;
assert(!libraryUi.includes(`title = "${legacyVisibleName}"`));
assert(!libraryUi.includes(`"Exit ${legacyVisibleName}"`));
assert(diagnostics.includes('BFPLAYER_FATAL_SIGNAL'));
assert(diagnostics.includes('BFPLAYER_LOG_TRUNCATED'));
assert(!libraryUi.includes('std::string("Sort: ")'));
assert(libraryUi.includes('library_item_name_less'));
assert(libraryUi.includes('previous_series'));
assert(libraryUi.includes('const bool empty = impl_->entries.empty();'));
assert(!libraryUi.includes(
    'const bool empty = impl_->entries.empty() && !impl_->scanning;'));
assert(libraryUi.includes(
    '"Square       Add Media: Import folder             Audio track"'));
const libraryCircleCase = libraryUi.slice(
    libraryUi.indexOf('case SDL_CONTROLLER_BUTTON_B:', libraryUi.indexOf(
        'bool play_queue = false;')),
    libraryUi.indexOf('case SDL_CONTROLLER_BUTTON_X:', libraryUi.indexOf(
        'bool play_queue = false;')));
assert(libraryCircleCase.includes('leave_season'));
assert(libraryCircleCase.includes('leave_series'));
assert(!libraryCircleCase.includes('play_queue'),
    'Circle must only navigate back and must never launch library media');
assert(libraryUi.includes('if (browsing && event.type == SDL_KEYDOWN)'));
assert(libraryUi.includes(
    '"Finish or cancel the current library import first"'));
assert(libraryUi.includes('SDL_CreateThread('));
assert(libraryUi.includes('"bfplayer-bulk-import"'));
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
assert(player.includes('create_subtitle_texture'),
    'subtitle switching updates only the subtitle texture');
assert(player.includes('track-switch refresh'),
    'audio and subtitle switches refresh around the current position');
assert(player.includes('PlaybackOverlay::subtitles'),
    'playback options expose a dedicated subtitle screen');
assert(player.includes('Browse subtitle files...'),
    'subtitle screen exposes the local subtitle browser');
assert(player.includes('Search SubDL by filename'),
    'subtitle screen exposes optional online search');
assert(player.includes('Search SubDL by title...'),
    'subtitle screen exposes manual-title fallback');
assert(player.includes('SubDL request failed - Circle: back'),
    'provider failures remain visible');
assert(subtitleBrowser.includes('dirfd(directory)'),
    'PS5 subtitle traversal uses the descriptor owned by DIR');
assert(player.includes('subtitle-provider search provider=subdl'),
    'online search emits credential-free diagnostics');
assert(!player.includes('api_key='),
    'player never adds provider API keys to URLs');
assert(!player.includes(
    'type == KIT_STREAMTYPE_VIDEO || type == KIT_STREAMTYPE_SUBTITLE'),
    'subtitle switching does not rebuild the video texture');
assert(demuxThread.includes(
    'Kit_SignalDemuxer(demuxer_thread->demuxer)'),
    'seek wakes a demuxer blocked by full packet buffers');
assert(!demuxThread.includes(
    'if(SDL_AtomicGet(&demuxer_thread->seek))\n        return;'),
    'a newer seek replaces a pending target instead of being discarded');
assert(player.includes(
    'Kit_SetHint(KIT_HINT_THREAD_COUNT, kVideoDecoderThreads)'));
assert(player.includes('constexpr int kVideoDecoderThreads = 16'));
assert(player.includes('frame_threading=%u slice_threading=%u'));
assert(kitchensinkDecoder.includes(
    'codec_ctx->thread_type |= FF_THREAD_FRAME'));
assert(kitchensinkDecoder.includes(
    'codec_ctx->thread_type |= FF_THREAD_SLICE'));
assert(kitchensinkVideoUtils.includes(
    'return SDL_PIXELFORMAT_IYUV'));
assert(player.includes('make_video_format_request'));
assert(player.includes(
    'video-output-request source=%dx%d output=%dx%d downscale=%d'));
assert(player.includes('migrate_legacy_library_database()'));
assert(player.includes(
    'Kit_SetHint(KIT_HINT_VIDEO_BUFFER_PACKETS, kVideoPacketBufferCount)'));
assert(player.includes(
    'Kit_SetHint(KIT_HINT_VIDEO_BUFFER_FRAMES, kVideoFrameBufferCount)'));
assert(player.includes('video_update_fps=%.2f'));
assert(player.includes('demanding_software_decode'));
assert(player.includes('display_aspect_from_sample_aspect('));
assert(player.includes('av_guess_sample_aspect_ratio('));
assert(player.includes(
    'app.video_scale_mode = bfplayer::VideoScaleMode::fit'));
assert(player.includes(
    'app.video_crop_mode = bfplayer::VideoCropMode::default_crop'));
assert(!player.includes(
    'resume_database.get_setting("video_scale_mode"'));
assert(!player.includes(
    'resume_database.get_setting("video_crop_mode"'));
assert(player.includes(
    '"playback-aspect frame=%dx%d sar=%d:%d sar_status=%d dar=%.6f"'));
assert(player.includes('sceSystemServiceGetAppIdOfRunningBigApp'));
assert(player.includes('sceSystemServiceGetAppTitleId'));
assert(player.includes('sceSystemServiceKillApp(app_id, -1, 0, 0)'));
assert(player.includes('return_to_playstation_home();'));
assert(player.includes('class PlayerInstanceLock'));
assert(player.includes('"/data/BFplayer/player.lock"'));
assert(player.includes('LOCK_EX | LOCK_NB'));
assert(player.includes('another player is already running'));
assert(player.includes('kBFplayerTitleId = "PSMC00001"'));
assert(player.includes('reason=unexpected-bigapp'));
assert(!player.includes('subtitle_delay_mode'),
    'D-pad seeking must not be captured by a hidden subtitle-delay mode');
assert(playbackOsd.includes('maximum_message_width'));
assert(playbackOsd.includes('fit_text_to_width('));
assert(ptraceCore.includes('snprintf(buf, sizeof(buf)'));
assert(!ptraceCore.includes('strcpy(buf, s)'));
assert(build.includes('bfplayer-standalone.elf'));
assert(build.includes(
    'vendor\\SDL_kitchensink\\src\\internal\\video\\kitvideo.c'));
assert(!build.includes("'lib\\libSDL_kitchensink.a'"));
assert(build.includes("'--strip-all'"));
assert(build.includes('GZipStream'));
assert(!build.includes(
    "$zip = Join-Path $distDir 'BFplayer-websrv.zip'"));
assert(!build.includes(
    "$tileInstaller = Join-Path $distDir 'bfplayer-tile-installer.elf'"));

console.log('standalone_launcher_tests: PASS');
