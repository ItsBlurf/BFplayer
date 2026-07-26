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
#include <sys/time.h>
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
#define PS5MC_LEGACY_HOST_TITLE_ID "PSMR00001"
#define PS5MC_LEGACY_HOST_APP_DIR \
    PS5MC_APP_ROOT "/" PS5MC_LEGACY_HOST_TITLE_ID
#define PS5MC_RUNTIME_DIR "/data/homebrew/PS5-MediaCenter"
#define PS5MC_FONT_DIR PS5MC_RUNTIME_DIR "/assets/fonts"
#define PS5MC_PLAYER_PATH PS5MC_RUNTIME_DIR "/ps5-media-center.elf"
#define PS5MC_LOG_DIR "/data/PS5-MediaCenter"
#define PS5MC_LOG_PATH PS5MC_LOG_DIR "/standalone-launcher.log"
#define PS5MC_PLAYER_LOG_PATH PS5MC_LOG_DIR "/player-stdio.log"
#define PS5MC_INSTANCE_LOCK_PATH PS5MC_LOG_DIR "/standalone-launcher.lock"
#define PS5MC_LEGACY_CLEAN_MARKER \
    PS5MC_RUNTIME_DIR "/.legacy-registration-cleaned"
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
int sceAppInstUtilAppUnInstall(const char*, void*, void*);
int sceKernelSendNotificationRequest(int, void*, size_t, int);

typedef struct ps5mc_notify_request {
    char reserved[45];
    char message[3075];
} ps5mc_notify_request_t;

static int mkdir_if_needed(const char* path) {
    struct stat info;

    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        if (lstat(path, &info) == 0 &&
            S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode)) {
            return 0;
        }
        return -ENOTDIR;
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

static void send_ready_notification(void) {
    ps5mc_notify_request_t request;
    int result;

    memset(&request, 0, sizeof(request));
    (void)snprintf(
        request.message,
        sizeof(request.message),
        "PS5 Media Center %s loaded. Open it from Media.",
        PS5MC_VERSION);
    result = sceKernelSendNotificationRequest(
        0, &request, sizeof(request), 0);
    launcher_log(
        "notification ready result=0x%08x",
        (unsigned int)result);
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

static int regular_file_matches(
    const char* path,
    const uint8_t* expected,
    size_t expected_size) {
    uint8_t buffer[16384];
    struct stat info;
    size_t offset = 0;
    int descriptor;

    if (!path || !expected) {
        return -EINVAL;
    }
    if (lstat(path, &info) != 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (!S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) {
        return -EINVAL;
    }
    if ((uintmax_t)info.st_size != (uintmax_t)expected_size) {
        return 0;
    }
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        return -errno;
    }
    while (offset < expected_size) {
        const size_t remaining = expected_size - offset;
        const size_t requested =
            remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const ssize_t got = read(descriptor, buffer, requested);
        if (got <= 0) {
            const int error = got == 0 ? EIO : errno;
            close(descriptor);
            return -error;
        }
        if (memcmp(expected + offset, buffer, (size_t)got) != 0) {
            close(descriptor);
            return 0;
        }
        offset += (size_t)got;
    }
    if (close(descriptor) != 0) {
        return -errno;
    }
    return 1;
}

static int update_file_atomic(
    const char* path,
    const uint8_t* bytes,
    size_t size,
    mode_t mode,
    int* changed) {
    const int matches = regular_file_matches(path, bytes, size);
    int result;

    if (matches < 0) {
        return matches;
    }
    if (matches > 0) {
        (void)chmod(path, mode);
        if (changed) {
            *changed = 0;
        }
        return 0;
    }
    result = write_file_atomic(path, bytes, size, mode);
    if (result == 0 && changed) {
        *changed = 1;
    }
    return result;
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
    int changed;
    int changed_files = 0;

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
    if ((result = update_file_atomic(
            PS5MC_FONT_DIR "/NotoSans-Regular.ttf",
            ps5mc_font_ttf,
            ps5mc_font_ttf_size,
            0644,
            &changed)) != 0) {
        return result;
    }
    changed_files += changed;
    if ((result = update_file_atomic(
            PS5MC_FONT_DIR "/OFL.txt",
            ps5mc_font_license,
            ps5mc_font_license_size,
            0644,
            &changed)) != 0) {
        return result;
    }
    changed_files += changed;
    if ((result = update_file_atomic(
            PS5MC_RUNTIME_DIR "/sce_sys/icon0.png",
            ps5mc_icon_png,
            ps5mc_icon_png_size,
            0644,
            &changed)) != 0) {
        return result;
    }
    changed_files += changed;
    if ((result = update_file_atomic(
            PS5MC_RUNTIME_DIR "/build-manifest.json",
            (const uint8_t*)manifest,
            (size_t)manifest_length,
            0644,
            &changed)) != 0) {
        return result;
    }
    changed_files += changed;
    launcher_log(
        "runtime assets version=%s changed_files=%d",
        PS5MC_VERSION,
        changed_files);
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
    int changed;
    int changed_files = 0;
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
        mkdir_if_needed(sce_sys_dir) != 0) {
        goto terminate_appinst;
    }
    if (update_file_atomic(
            param_path,
            ps5mc_tile_param_json,
            ps5mc_tile_param_json_size,
            0644,
            &changed) != 0) {
        goto terminate_appinst;
    }
    changed_files += changed;
    if (update_file_atomic(
            icon_path,
            ps5mc_icon_png,
            ps5mc_icon_png_size,
            0644,
            &changed) != 0) {
        goto terminate_appinst;
    }
    changed_files += changed;
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
        "tile result=%d changed_files=%d title_dir=0x%08x install_all=0x%08x",
        result,
        changed_files,
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

static void remove_legacy_bigapp_host_registration(void) {
    static const uint8_t clean_marker[] = "PSMR00001 removed\n";
    struct stat legacy_info;
    struct stat marker_info;
    const pid_t pid = getpid();
    const uint64_t original_authid = kernel_get_ucred_authid(pid);
    int uninstall_rc = PS5MC_DIAG_SKIPPED;

    if (lstat(PS5MC_LEGACY_CLEAN_MARKER, &marker_info) == 0 &&
        S_ISREG(marker_info.st_mode) && !S_ISLNK(marker_info.st_mode) &&
        lstat(
            PS5MC_LEGACY_HOST_APP_DIR "/sce_sys/param.json",
            &legacy_info) != 0 &&
        errno == ENOENT) {
        launcher_log("legacy host cleanup already complete; skipping");
        return;
    }
    if (kernel_set_ucred_authid(pid, PS5MC_APPINST_AUTHID) != 0) {
        launcher_log("legacy host uninstall authid swap failed");
        return;
    }
    uninstall_rc = sceAppInstUtilInitialize();
    if (uninstall_rc != 0) {
        launcher_log(
            "legacy host uninstall AppInst initialize failed rc=0x%08x",
            uninstall_rc);
        goto cleanup;
    }
    uninstall_rc = sceAppInstUtilAppUnInstall(
        PS5MC_LEGACY_HOST_TITLE_ID,
        NULL,
        NULL);
    // Sony's install/uninstall paths touch the same app database. Give the
    // shell time to consume the removal before registering PSMC00001.
    usleep(400000);
    // Prevent AppInstallAll fallback from rediscovering stale alpha.11/12
    // metadata if Sony's uninstall left the title directory behind.
    if (unlink(PS5MC_LEGACY_HOST_APP_DIR "/sce_sys/param.json") != 0 &&
        errno != ENOENT) {
        launcher_log("legacy host param remove failed errno=%d", errno);
    }
    if (unlink(PS5MC_LEGACY_HOST_APP_DIR "/sce_sys/icon0.png") != 0 &&
        errno != ENOENT) {
        launcher_log("legacy host icon remove failed errno=%d", errno);
    }
    if (rmdir(PS5MC_LEGACY_HOST_APP_DIR "/sce_sys") != 0 &&
        errno != ENOENT && errno != ENOTEMPTY) {
        launcher_log("legacy host sce_sys remove failed errno=%d", errno);
    }
    if (rmdir(PS5MC_LEGACY_HOST_APP_DIR) != 0 &&
        errno != ENOENT && errno != ENOTEMPTY) {
        launcher_log("legacy host directory remove failed errno=%d", errno);
    }
    launcher_log(
        "legacy host uninstall title=%s rc=0x%08x",
        PS5MC_LEGACY_HOST_TITLE_ID,
        uninstall_rc);
    (void)sceAppInstUtilTerminate();
    if (write_file_atomic(
            PS5MC_LEGACY_CLEAN_MARKER,
            clean_marker,
            sizeof(clean_marker) - 1U,
            0600) != 0) {
        launcher_log("legacy host cleanup marker write failed errno=%d", errno);
    }
cleanup:
    if (original_authid != 0) {
        (void)kernel_set_ucred_authid(pid, original_authid);
    }
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
        const int error = errno;
        close(listener);
        errno = error;
        return -1;
    }
    return listener;
}

static int request_existing_launcher_shutdown(void) {
    static const char request[] =
        "GET /shutdown HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    struct sockaddr_in address;
    char response[128];
    int connection = socket(AF_INET, SOCK_STREAM, 0);
    size_t sent = 0;
    ssize_t received;
    struct timeval timeout = {1, 0};

    if (connection < 0) {
        return -1;
    }
    (void)setsockopt(
        connection,
        SOL_SOCKET,
        SO_SNDTIMEO,
        &timeout,
        sizeof(timeout));
    (void)setsockopt(
        connection,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PS5MC_SERVICE_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(
            connection,
            (struct sockaddr*)&address,
            sizeof(address)) != 0) {
        close(connection);
        return -1;
    }
    while (sent < sizeof(request) - 1U) {
        const ssize_t written = send(
            connection,
            request + sent,
            sizeof(request) - 1U - sent,
            0);
        if (written <= 0) {
            close(connection);
            return -1;
        }
        sent += (size_t)written;
    }
    received = recv(connection, response, sizeof(response) - 1U, 0);
    close(connection);
    if (received <= 0) {
        return -1;
    }
    response[received] = '\0';
    return strstr(response, "HTTP/1.1 200 ") == response ? 0 : -1;
}

static int acquire_instance_lock(void) {
    char owner[128];
    int descriptor;
    int attempt;
    int length;

    (void)mkdir_if_needed("/data");
    if (mkdir_if_needed(PS5MC_LOG_DIR) != 0) {
        return -1;
    }
    descriptor = open(
        PS5MC_INSTANCE_LOCK_PATH,
        O_RDWR | O_CREAT,
        0600);
    if (descriptor < 0) {
        return -1;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            const int error = errno;
            close(descriptor);
            errno = error;
            return -1;
        }
        launcher_log(
            "existing locked launcher detected; requesting serialized takeover");
        if (request_existing_launcher_shutdown() != 0) {
            launcher_log("locked launcher graceful shutdown request failed");
            close(descriptor);
            errno = EADDRINUSE;
            return -1;
        }
        for (attempt = 0; attempt < 75; ++attempt) {
            usleep(40000);
            if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
                break;
            }
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                const int error = errno;
                close(descriptor);
                errno = error;
                return -1;
            }
        }
        if (attempt == 75) {
            launcher_log("serialized takeover lock timed out");
            close(descriptor);
            errno = EADDRINUSE;
            return -1;
        }
        launcher_log("serialized takeover lock acquired attempts=%d", attempt + 1);
    } else {
        launcher_log("instance lock acquired");
    }

    length = snprintf(
        owner,
        sizeof(owner),
        "pid=%ld version=%s\n",
        (long)getpid(),
        PS5MC_VERSION);
    if (length > 0 && length < (int)sizeof(owner) &&
        ftruncate(descriptor, 0) == 0 &&
        lseek(descriptor, 0, SEEK_SET) == 0) {
        (void)write(descriptor, owner, (size_t)length);
        (void)fsync(descriptor);
    }
    return descriptor;
}

static int create_loopback_listener_with_takeover(void) {
    int listener = create_loopback_listener();
    int attempt;
    if (listener >= 0 || errno != EADDRINUSE) {
        return listener;
    }
    launcher_log("existing launcher detected; requesting graceful takeover");
    if (request_existing_launcher_shutdown() != 0) {
        launcher_log("existing launcher does not support graceful takeover");
        errno = EADDRINUSE;
        return -1;
    }
    for (attempt = 0; attempt < 50; ++attempt) {
        usleep(40000);
        listener = create_loopback_listener();
        if (listener >= 0) {
            launcher_log("graceful takeover complete attempts=%d", attempt + 1);
            return listener;
        }
        if (errno != EADDRINUSE) {
            return -1;
        }
    }
    launcher_log("graceful takeover timed out");
    errno = EADDRINUSE;
    return -1;
}

static void send_response(
    int connection,
    int status,
    const char* content_type,
    const char* body) {
    char header[512];
    const size_t body_length = body ? strlen(body) : 0U;
    const int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status,
        status == 200 ? "OK" : "Not Found",
        content_type,
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
    static const char launch_page[] =
        "<!doctype html><html><head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>PS5 Media Center</title>"
        "<style>"
        "html,body{height:100%;margin:0;background:#050913;color:#f0f4ff;"
        "font-family:Arial,sans-serif}body{display:flex;align-items:center;"
        "justify-content:center;text-align:center}.card{width:min(760px,80vw);"
        "padding:64px;border:1px solid #192a47;background:#0b1425;"
        "box-shadow:0 24px 80px #000}.accent{height:4px;background:#2f89ff;"
        "margin:0 auto 38px;width:120px}h1{font-size:42px;margin:0 0 20px}"
        "p{font-size:24px;color:#9daac4;line-height:1.5}.count{color:#f4b22a;"
        "font-size:72px;font-weight:bold;margin:18px}</style></head><body>"
        "<main class=\"card\"><div class=\"accent\"></div>"
        "<h1>Launching PS5 Media Center</h1>"
        "<p>The player is starting. Closing this window automatically...</p>"
        "<div class=\"count\" id=\"count\">2</div>"
        "<p id=\"fallback\">If this window remains, press O to close it.</p>"
        "</main><script>"
        "let n=2;const e=document.getElementById('count');"
        "function leave(){"
        "try{window.open('','_self')}catch(x){};"
        "try{window.close()}catch(x){};"
        "try{self.close()}catch(x){};"
        "try{history.back()}catch(x){};"
        "try{history.go(-1)}catch(x){}"
        "}"
        "const t=setInterval(()=>{n--;e.textContent=n>0?n:'Ready';"
        "if(n<=0){clearInterval(t);leave();"
        "setInterval(leave,750)}},750);"
        "</script></body></html>";
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
            send_response(connection, 200, "text/html", launch_page);
            close(connection);
            launcher_log("request route=/launch");
            (void)launch_embedded_player();
        } else if (ps5mc_request_is_shutdown(request)) {
            send_response(connection, 200, "text/plain", "Shutting down\n");
            close(connection);
            launcher_log("request route=/shutdown action=exit");
            return;
        } else {
            send_response(connection, 404, "text/plain", "Not found\n");
            close(connection);
        }
    }
}

int main(void) {
    int instance_lock;
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
    instance_lock = acquire_instance_lock();
    if (instance_lock < 0) {
        launcher_log("instance ownership failed errno=%d", errno);
        return 7;
    }
    listener = create_loopback_listener_with_takeover();
    if (listener < 0) {
        launcher_log(
            "loopback takeover failed address=%s port=%d errno=%d",
            PS5MC_SERVICE_ADDRESS,
            PS5MC_SERVICE_PORT,
            errno);
        close(instance_lock);
        return 5;
    }
    // The old service is gone and this process owns the lifetime lock. Close
    // the temporary port reservation while updating so a simultaneous
    // injector cannot queue a shutdown request that would be consumed only
    // after this process becomes ready.
    close(listener);
    launcher_log("loopback handover complete; applying serialized update");
    if ((result = install_runtime_assets()) != 0) {
        launcher_log("runtime asset install failed rc=%d", result);
        close(instance_lock);
        return 3;
    }
    (void)sceUserServiceInitialize(NULL);
    remove_legacy_bigapp_host_registration();
    if (hbldr_prepare_host() != 0) {
        launcher_log("BigApp system host preparation failed errno=%d", errno);
        close(instance_lock);
        (void)sceUserServiceTerminate();
        return 4;
    }
    if (install_dashboard_tile() != 0) {
        launcher_log("dashboard tile install failed");
        close(instance_lock);
        (void)sceUserServiceTerminate();
        return 6;
    }
    listener = create_loopback_listener();
    if (listener < 0) {
        launcher_log(
            "final loopback bind failed address=%s port=%d errno=%d",
            PS5MC_SERVICE_ADDRESS,
            PS5MC_SERVICE_PORT,
            errno);
        close(instance_lock);
        (void)sceUserServiceTerminate();
        return 5;
    }
    launcher_log(
        "ready address=%s port=%d routes=/launch,/shutdown websrv=unused",
        PS5MC_SERVICE_ADDRESS,
        PS5MC_SERVICE_PORT);
    send_ready_notification();
    serve_forever(listener);
    close(listener);
    (void)flock(instance_lock, LOCK_UN);
    close(instance_lock);
    (void)sceUserServiceTerminate();
    payload_exit(0);
    return 0;
}
