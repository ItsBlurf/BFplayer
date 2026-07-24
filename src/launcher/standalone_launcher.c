/*
 * PS5 Media Center standalone launcher.
 *
 * One resident payload contains the player ELF, installs the dashboard tile
 * and runtime assets, then serves one loopback-only launch route. It does not
 * embed, link, start, or communicate with ps5-payload-websrv.
 *
 * The BigApp loader core under launcher/core is derived from John Törnblom's
 * ps5-payload-websrv and remains GPL-3.0-or-later. This combined payload is
 * distributed under the same license; see LICENSE.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#include "ps5mc/standalone_route.h"
#include "core/hbldr.h"
#include "core/standalone_fs.h"

#define PS5MC_TITLE_ID "PSMC00001"
#define PS5MC_SERVICE_PORT 9040
#define PS5MC_SERVICE_ADDRESS "127.0.0.1"
#define PS5MC_APP_ROOT "/user/app"
#define PS5MC_APP_PARENT PS5MC_APP_ROOT "/"
#define PS5MC_APP_DIR PS5MC_APP_ROOT "/" PS5MC_TITLE_ID
#define PS5MC_HOST_TITLE_ID "PSMR00001"
#define PS5MC_HOST_APP_DIR PS5MC_APP_ROOT "/" PS5MC_HOST_TITLE_ID
#define PS5MC_RUNTIME_DIR "/data/homebrew/PS5-MediaCenter"
#define PS5MC_FONT_DIR PS5MC_RUNTIME_DIR "/assets/fonts"
#define PS5MC_PLAYER_PATH PS5MC_RUNTIME_DIR "/ps5-media-center.elf"
#define PS5MC_LOG_DIR "/data/PS5-MediaCenter"
#define PS5MC_LOG_PATH PS5MC_LOG_DIR "/standalone-launcher.log"
#define PS5MC_PLAYER_LOG_PATH PS5MC_LOG_DIR "/player-stdio.log"
#define PS5MC_APPINST_AUTHID UINT64_C(0x4801000000000013)
#define PS5MC_DIAG_SKIPPED (-2147483000)

#ifndef PS5MC_VERSION
#define PS5MC_VERSION "development"
#endif

#define INCASSET(name, file)                                                   \
    __asm__(".section .rodata\n"                                               \
            ".global " #name "\n" #name ":\n"                                \
            ".incbin \"" file "\"\n" #name "_end:\n"                         \
            ".global " #name "_size\n" #name "_size:\n"                      \
            ".quad " #name "_end - " #name "\n");                            \
    extern const uint8_t name[];                                               \
    extern const unsigned long name##_size

INCASSET(ps5mc_embedded_player_gzip, "build/ps5/ps5-media-center.elf.gz");
INCASSET(ps5mc_tile_param_json, "assets/tile/param.json");
INCASSET(ps5mc_host_param_json, "assets/fakeapp/param.json");
INCASSET(ps5mc_icon_png, "assets/icon0.png");
INCASSET(ps5mc_font_ttf, "assets/fonts/NotoSans-Regular.ttf");
INCASSET(ps5mc_font_license, "assets/fonts/OFL.txt");

typedef int (*app_install_title_dir_fn)(
    const char* title_id,
    const char* app_root,
    void* reserved);

int sceUserServiceInitialize(void*);
int sceUserServiceTerminate(void);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void*);

static int mkdir_if_needed(const char* path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    return -errno;
}

static void launcher_log(const char* format, ...) {
    char body[1024];
    char line[1280];
    va_list args;
    int length;
    int descriptor;

    va_start(args, format);
    vsnprintf(body, sizeof(body), format ? format : "", args);
    va_end(args);
    length = snprintf(
        line,
        sizeof(line),
        "%ld pid=%ld %s\n",
        (long)time(NULL),
        (long)getpid(),
        body);
    if (length <= 0) {
        return;
    }
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
    }
    fputs(line, stdout);
    fflush(stdout);
    (void)mkdir_if_needed("/data");
    (void)mkdir_if_needed(PS5MC_LOG_DIR);
    descriptor = open(PS5MC_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (descriptor >= 0) {
        (void)write(descriptor, line, (size_t)length);
        close(descriptor);
    }
}

static int write_file_atomic(
    const char* path,
    const uint8_t* bytes,
    size_t size,
    mode_t mode) {
    char temporary[512];
    size_t offset = 0;
    int descriptor;

    if (!path || !bytes || snprintf(
            temporary,
            sizeof(temporary),
            "%s.tmp.%ld",
            path,
            (long)getpid()) >= (int)sizeof(temporary)) {
        return -EINVAL;
    }
    descriptor = open(
        temporary,
        O_WRONLY | O_CREAT | O_TRUNC,
        mode);
    if (descriptor < 0) {
        return -errno;
    }
    while (offset < size) {
        const ssize_t written = write(descriptor, bytes + offset, size - offset);
        if (written <= 0) {
            const int error = written == 0 ? ENOSPC : errno;
            close(descriptor);
            unlink(temporary);
            return -error;
        }
        offset += (size_t)written;
    }
    if (fsync(descriptor) != 0) {
        const int error = errno;
        close(descriptor);
        unlink(temporary);
        return -error;
    }
    if (close(descriptor) != 0) {
        const int error = errno;
        unlink(temporary);
        return -error;
    }
    if (rename(temporary, path) != 0) {
        const int error = errno;
        unlink(temporary);
        return -error;
    }
    (void)chmod(path, mode);
    return 0;
}

uint8_t* fs_readfile(const char* path, size_t* size) {
    struct stat info;
    uint8_t* bytes = NULL;
    size_t offset = 0;
    int descriptor;

    if (!path || stat(path, &info) != 0 || info.st_size <= 0) {
        return NULL;
    }
    if ((uintmax_t)info.st_size > (uintmax_t)SIZE_MAX - 1U) {
        errno = EFBIG;
        return NULL;
    }
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        return NULL;
    }
    bytes = malloc((size_t)info.st_size + 1U);
    if (!bytes) {
        close(descriptor);
        return NULL;
    }
    while (offset < (size_t)info.st_size) {
        const ssize_t got = read(
            descriptor,
            bytes + offset,
            (size_t)info.st_size - offset);
        if (got <= 0) {
            const int error = got == 0 ? EIO : errno;
            free(bytes);
            close(descriptor);
            errno = error;
            return NULL;
        }
        offset += (size_t)got;
    }
    close(descriptor);
    bytes[offset] = 0;
    if (size) {
        *size = offset;
    }
    return bytes;
}

static int install_runtime_assets(void) {
    char manifest[512];
    const int manifest_length = snprintf(
        manifest,
        sizeof(manifest),
        "{\n"
        "  \"name\": \"PS5 Media Center\",\n"
        "  \"version\": \"%s\",\n"
        "  \"launch\": \"embedded-standalone\",\n"
        "  \"loopback\": \"%s:%d\",\n"
        "  \"websrv\": false\n"
        "}\n",
        PS5MC_VERSION,
        PS5MC_SERVICE_ADDRESS,
        PS5MC_SERVICE_PORT);
    int result;

    if (manifest_length <= 0 || manifest_length >= (int)sizeof(manifest)) {
        return -EOVERFLOW;
    }
    if ((result = mkdir_if_needed("/data")) != 0 ||
        (result = mkdir_if_needed("/data/homebrew")) != 0 ||
        (result = mkdir_if_needed(PS5MC_RUNTIME_DIR)) != 0 ||
        (result = mkdir_if_needed(PS5MC_RUNTIME_DIR "/assets")) != 0 ||
        (result = mkdir_if_needed(PS5MC_FONT_DIR)) != 0 ||
        (result = mkdir_if_needed(PS5MC_RUNTIME_DIR "/sce_sys")) != 0) {
        return result;
    }
    if ((result = write_file_atomic(
            PS5MC_FONT_DIR "/NotoSans-Regular.ttf",
            ps5mc_font_ttf,
            ps5mc_font_ttf_size,
            0644)) != 0 ||
        (result = write_file_atomic(
            PS5MC_FONT_DIR "/OFL.txt",
            ps5mc_font_license,
            ps5mc_font_license_size,
            0644)) != 0 ||
        (result = write_file_atomic(
            PS5MC_RUNTIME_DIR "/sce_sys/icon0.png",
            ps5mc_icon_png,
            ps5mc_icon_png_size,
            0644)) != 0 ||
        (result = write_file_atomic(
            PS5MC_RUNTIME_DIR "/build-manifest.json",
            (const uint8_t*)manifest,
            (size_t)manifest_length,
            0644)) != 0) {
        return result;
    }
    return 0;
}

static int install_dashboard_tile(void) {
    char sce_sys_dir[256];
    char param_path[320];
    char icon_path[320];
    uint32_t appinst_handle = 0;
    app_install_title_dir_fn install_title_dir = NULL;
    const pid_t pid = getpid();
    const uint64_t original_authid = kernel_get_ucred_authid(pid);
    int title_dir_rc = PS5MC_DIAG_SKIPPED;
    int install_all_rc = PS5MC_DIAG_SKIPPED;
    int result = -1;

    if (kernel_set_ucred_authid(pid, PS5MC_APPINST_AUTHID) != 0) {
        return -1;
    }
    if (sceAppInstUtilInitialize() != 0) {
        goto cleanup;
    }
    snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", PS5MC_APP_DIR);
    snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
    snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);
    if (mkdir_if_needed(PS5MC_APP_DIR) != 0 ||
        mkdir_if_needed(sce_sys_dir) != 0 ||
        write_file_atomic(
            param_path,
            ps5mc_tile_param_json,
            ps5mc_tile_param_json_size,
            0644) != 0 ||
        write_file_atomic(
            icon_path,
            ps5mc_icon_png,
            ps5mc_icon_png_size,
            0644) != 0) {
        goto terminate_appinst;
    }
    if (kernel_dynlib_handle(
            -1,
            "libSceAppInstUtil.sprx",
            &appinst_handle) == 0) {
        install_title_dir =
            (app_install_title_dir_fn)kernel_dynlib_resolve(
                -1,
                appinst_handle,
                "Wudg3Xe3heE");
    }
    if (install_title_dir) {
        title_dir_rc = install_title_dir(
            PS5MC_TITLE_ID,
            PS5MC_APP_PARENT,
            NULL);
    }
    if (title_dir_rc != 0) {
        install_all_rc = sceAppInstUtilAppInstallAll(NULL);
    }
    if (title_dir_rc == 0 || install_all_rc == 0) {
        result = 0;
    }
    launcher_log(
        "tile result=%d title_dir=0x%08x install_all=0x%08x",
        result,
        title_dir_rc,
        install_all_rc);

terminate_appinst:
    (void)sceAppInstUtilTerminate();
cleanup:
    if (original_authid != 0) {
        (void)kernel_set_ucred_authid(pid, original_authid);
    }
    return result;
}

static int install_bigapp_host_registration(void) {
    char sce_sys_dir[256];
    char param_path[320];
    char icon_path[320];
    uint32_t appinst_handle = 0;
    app_install_title_dir_fn install_title_dir = NULL;
    const pid_t pid = getpid();
    const uint64_t original_authid = kernel_get_ucred_authid(pid);
    int title_dir_rc = PS5MC_DIAG_SKIPPED;
    int install_all_rc = PS5MC_DIAG_SKIPPED;
    int result = -1;

    if (kernel_set_ucred_authid(pid, PS5MC_APPINST_AUTHID) != 0) {
        return -1;
    }
    if (sceAppInstUtilInitialize() != 0) {
        goto cleanup;
    }
    snprintf(
        sce_sys_dir,
        sizeof(sce_sys_dir),
        "%s/sce_sys",
        PS5MC_HOST_APP_DIR);
    snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
    snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);
    if (mkdir_if_needed(PS5MC_HOST_APP_DIR) != 0 ||
        mkdir_if_needed(sce_sys_dir) != 0 ||
        write_file_atomic(
            param_path,
            ps5mc_host_param_json,
            ps5mc_host_param_json_size,
            0644) != 0 ||
        write_file_atomic(
            icon_path,
            ps5mc_icon_png,
            ps5mc_icon_png_size,
            0644) != 0) {
        goto terminate_appinst;
    }
    if (kernel_dynlib_handle(
            -1,
            "libSceAppInstUtil.sprx",
            &appinst_handle) == 0) {
        install_title_dir =
            (app_install_title_dir_fn)kernel_dynlib_resolve(
                -1,
                appinst_handle,
                "Wudg3Xe3heE");
    }
    if (install_title_dir) {
        title_dir_rc = install_title_dir(
            PS5MC_HOST_TITLE_ID,
            PS5MC_APP_PARENT,
            NULL);
    }
    if (title_dir_rc != 0) {
        install_all_rc = sceAppInstUtilAppInstallAll(NULL);
    }
    if (title_dir_rc == 0 || install_all_rc == 0) {
        result = 0;
    }
    launcher_log(
        "host registration result=%d title_dir=0x%08x install_all=0x%08x",
        result,
        title_dir_rc,
        install_all_rc);

terminate_appinst:
    (void)sceAppInstUtilTerminate();
cleanup:
    if (original_authid != 0) {
        (void)kernel_set_ucred_authid(pid, original_authid);
    }
    return result;
}

static int create_loopback_listener(void) {
    struct sockaddr_in address;
    int listener;
    int enabled = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return -1;
    }
    (void)setsockopt(
        listener,
        SOL_SOCKET,
        SO_REUSEADDR,
        &enabled,
        sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PS5MC_SERVICE_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 2) != 0) {
        close(listener);
        return -1;
    }
    return listener;
}

static void send_response(int connection, int status, const char* body) {
    char header[512];
    const size_t body_length = body ? strlen(body) : 0U;
    const int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status,
        status == 200 ? "OK" : "Not Found",
        body_length);
    if (header_length > 0 && header_length < (int)sizeof(header)) {
        (void)send(connection, header, (size_t)header_length, 0);
    }
    if (body_length > 0) {
        (void)send(connection, body, body_length, 0);
    }
}

static int launch_embedded_player(void) {
    char* argv[] = {NULL};
    char* envp[] = {NULL};
    uint8_t* player = NULL;
    z_stream stream;
    int inflate_result;
    int stdio_descriptor = open(
        PS5MC_PLAYER_LOG_PATH,
        O_WRONLY | O_CREAT | O_APPEND,
        0600);
    pid_t player_pid;

    if (install_runtime_assets() != 0) {
        launcher_log(
            "runtime asset refresh before launch failed errno=%d",
            errno);
        if (stdio_descriptor >= 0) {
            close(stdio_descriptor);
        }
        return -1;
    }
    memset(&stream, 0, sizeof(stream));
    player = malloc((size_t)PS5MC_PLAYER_UNCOMPRESSED_SIZE);
    if (!player) {
        launcher_log(
            "player decompress allocation failed bytes=%lu",
            (unsigned long)PS5MC_PLAYER_UNCOMPRESSED_SIZE);
        if (stdio_descriptor >= 0) {
            close(stdio_descriptor);
        }
        return -1;
    }
    stream.next_in = (Bytef*)ps5mc_embedded_player_gzip;
    stream.avail_in = (uInt)ps5mc_embedded_player_gzip_size;
    stream.next_out = player;
    stream.avail_out = (uInt)PS5MC_PLAYER_UNCOMPRESSED_SIZE;
    inflate_result = inflateInit2(&stream, 15 + 16);
    if (inflate_result == Z_OK) {
        inflate_result = inflate(&stream, Z_FINISH);
        (void)inflateEnd(&stream);
    }
    if (inflate_result != Z_STREAM_END ||
        stream.total_out != (uLong)PS5MC_PLAYER_UNCOMPRESSED_SIZE ||
        stream.total_in != (uLong)ps5mc_embedded_player_gzip_size ||
        player[0] != 0x7f || player[1] != 'E' ||
        player[2] != 'L' || player[3] != 'F' || player[4] != 2U) {
        launcher_log(
            "player decompress failed zlib=%d in=%lu/%lu out=%lu/%lu",
            inflate_result,
            (unsigned long)stream.total_in,
            ps5mc_embedded_player_gzip_size,
            (unsigned long)stream.total_out,
            (unsigned long)PS5MC_PLAYER_UNCOMPRESSED_SIZE);
        free(player);
        if (stdio_descriptor >= 0) {
            close(stdio_descriptor);
        }
        return -1;
    }
    player_pid = hbldr_launch_buffer(
        PS5MC_RUNTIME_DIR,
        PS5MC_PLAYER_PATH,
        stdio_descriptor,
        argv,
        envp,
        player);
    free(player);
    if (stdio_descriptor >= 0) {
        close(stdio_descriptor);
    }
    launcher_log(
        "launch result=%s player_pid=%ld compressed_bytes=%lu player_bytes=%lu",
        player_pid > 0 ? "started" : "failed",
        (long)player_pid,
        ps5mc_embedded_player_gzip_size,
        (unsigned long)PS5MC_PLAYER_UNCOMPRESSED_SIZE);
    return player_pid > 0 ? 0 : -1;
}

static void serve_forever(int listener) {
    for (;;) {
        char request[2048];
        int connection = accept(listener, NULL, NULL);
        ssize_t received;

        if (connection < 0) {
            if (errno == EINTR) {
                continue;
            }
            launcher_log("accept failed errno=%d", errno);
            continue;
        }
#ifdef SO_NOSIGPIPE
        {
            int enabled = 1;
            (void)setsockopt(
                connection,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &enabled,
                sizeof(enabled));
        }
#endif
        received = recv(connection, request, sizeof(request) - 1U, 0);
        if (received > 0) {
            request[received] = '\0';
        } else {
            request[0] = '\0';
        }
        if (ps5mc_request_is_launch(request)) {
            send_response(connection, 200, "Launching PS5 Media Center\n");
            close(connection);
            launcher_log("request route=/launch");
            (void)launch_embedded_player();
        } else {
            send_response(connection, 404, "Not found\n");
            close(connection);
        }
    }
}

int main(void) {
    int listener;
    int result;

    (void)signal(SIGPIPE, SIG_IGN);
    launcher_log(
        "start version=%s address=%s port=%d player_bytes=%lu",
        PS5MC_VERSION,
        PS5MC_SERVICE_ADDRESS,
        PS5MC_SERVICE_PORT,
        (unsigned long)PS5MC_PLAYER_UNCOMPRESSED_SIZE);
    if (ps5mc_embedded_player_gzip_size < 64U ||
        ps5mc_embedded_player_gzip[0] != 0x1f ||
        ps5mc_embedded_player_gzip[1] != 0x8b) {
        launcher_log("embedded player gzip validation failed");
        return 2;
    }
    if ((result = install_runtime_assets()) != 0) {
        launcher_log("runtime asset install failed rc=%d", result);
        return 3;
    }
    (void)sceUserServiceInitialize(NULL);
    if (install_bigapp_host_registration() != 0) {
        launcher_log("BigApp host registration failed");
        (void)sceUserServiceTerminate();
        return 4;
    }
    if (hbldr_prepare_host() != 0) {
        launcher_log("BigApp system host preparation failed errno=%d", errno);
        (void)sceUserServiceTerminate();
        return 5;
    }
    listener = create_loopback_listener();
    if (listener < 0) {
        launcher_log(
            "loopback bind failed address=%s port=%d errno=%d",
            PS5MC_SERVICE_ADDRESS,
            PS5MC_SERVICE_PORT,
            errno);
        (void)sceUserServiceTerminate();
        return 6;
    }
    if (install_dashboard_tile() != 0) {
        launcher_log("dashboard tile install failed");
        close(listener);
        (void)sceUserServiceTerminate();
        return 7;
    }
    launcher_log(
        "ready address=%s port=%d route=/launch websrv=unused",
        PS5MC_SERVICE_ADDRESS,
        PS5MC_SERVICE_PORT);
    serve_forever(listener);
    close(listener);
    (void)sceUserServiceTerminate();
    payload_exit(0);
    return 0;
}
