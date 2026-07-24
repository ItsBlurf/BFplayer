/* PS5 Media Center websrv launcher. */

const DIRECT_STREAM_PROTOCOLS = new Set([
    'ftp:',
    'http:',
    'https:',
    'mmsh:',
    'mmst:',
    'rtmp:',
    'rtmpe:',
    'rtmps:',
    'rtmpt:',
    'rtmpte:',
    'rtmpts:',
    'rtp:',
    'rtsp:',
    'sctp:',
    'srtp:',
    'tcp:',
    'udp:',
    'udplite:'
]);

function validateDirectStreamUrl(value) {
    const candidate = (value || '').trim();
    if (!candidate) {
        return null;
    }

    let parsed;
    try {
        parsed = new URL(candidate);
    } catch (error) {
        alert('Enter a complete network URL, for example https://host/video.m3u8');
        return null;
    }

    if (!DIRECT_STREAM_PROTOCOLS.has(parsed.protocol.toLowerCase()) || !parsed.hostname) {
        alert('Unsupported stream protocol. Use HTTP(S), FTP, RTSP, RTMP, RTP, TCP, UDP, SCTP, or MMST/MMSh.');
        return null;
    }
    if (parsed.username || parsed.password) {
        alert('URLs containing usernames or passwords are not accepted. Use a credential-free trusted-LAN URL or the websrv network-share picker.');
        return null;
    }
    return candidate;
}

async function main() {
    const workdir = window.workingDir;
    const payload = workdir + '/ps5-media-center.elf';

    return {
        mainText: 'PS5 Media Center',
        secondaryText: 'Video, audio, subtitles, and media library',
        onclick: async () => ({
            path: payload,
            cwd: workdir,
            args: []
        }),
        options: [
            {
                text: 'Add a movie file to the library',
                onclick: async () => {
                    const file = await pickFile('', 'Select one movie...', false);
                    if (!file || !file.startsWith('/')) {
                        return;
                    }
                    return {
                        path: payload,
                        cwd: workdir,
                        args: ['--add-movie', file]
                    };
                }
            },
            {
                text: 'Add a TV show folder to the library',
                onclick: async () => {
                    const folder = await pickDirectory(
                        '',
                        'Select a folder to treat as one TV show...',
                        false);
                    if (!folder || !folder.startsWith('/')) {
                        return;
                    }
                    return {
                        path: payload,
                        cwd: workdir,
                        args: ['--add-tv-folder', folder]
                    };
                }
            },
            {
                text: 'Play a file directly',
                onclick: async () => {
                    let file = await pickFile('', 'Select media...', true);
                    if (!file) {
                        return;
                    }
                    if (!file.startsWith('/')) {
                        file = ApiClient.getNetworkShareHttpProxyUrl(file);
                    }
                    return {
                        path: payload,
                        cwd: workdir,
                        args: [file]
                    };
                }
            },
            {
                text: 'Play a file with external subtitles',
                onclick: async () => {
                    let file = await pickFile('', 'Select media...', true);
                    if (!file) {
                        return;
                    }
                    const folder = file.substring(0, file.lastIndexOf('/'));
                    let subtitle = await pickFile(folder, 'Select subtitles...', true);
                    if (!subtitle) {
                        return;
                    }
                    if (!file.startsWith('/')) {
                        file = ApiClient.getNetworkShareHttpProxyUrl(file);
                    }
                    if (!subtitle.startsWith('/')) {
                        subtitle = ApiClient.getNetworkShareHttpProxyUrl(subtitle);
                    }
                    return {
                        path: payload,
                        cwd: workdir,
                        args: [file, '--subtitle', subtitle]
                    };
                }
            },
            {
                text: 'Play a network URL',
                onclick: async () => {
                    const stream = validateDirectStreamUrl(
                        prompt('Enter a media stream URL (HTTP/HLS, RTSP, RTMP, RTP, UDP, TCP, FTP, or MMST):', 'https://'));
                    if (!stream) {
                        return;
                    }
                    return {
                        path: payload,
                        cwd: workdir,
                        args: [stream]
                    };
                }
            }
        ]
    };
}
