/*
 * PS5 Media Center direct-launch tile installer.
 *
 * This privileged helper is deliberately separate from the media player. It
 * registers a Media-category HTTP tile that calls websrv's /hbldr endpoint
 * with the exact PS5 Media Center BigApp ELF. The catalog UI and picker are
 * never opened.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define PS5MC_TITLE_ID "PSMC00001"
#define PS5MC_APP_ROOT "/user/app"
#define PS5MC_APP_PARENT PS5MC_APP_ROOT "/"
#define PS5MC_APP_DIR PS5MC_APP_ROOT "/" PS5MC_TITLE_ID
#define PS5MC_LOG_DIR "/data/PS5-MediaCenter"
#define PS5MC_LOG_PATH PS5MC_LOG_DIR "/tile-installer.log"
#define PS5MC_APPINST_AUTHID UINT64_C(0x4801000000000013)
#define PS5MC_DIAG_SKIPPED (-2147483000)

#define INCASSET(name, file)                                                   \
    __asm__(".section .rodata\n"                                               \
            ".global " #name "\n" #name ":\n"                                \
            ".incbin \"" file "\"\n" #name "_end:\n"                         \
            ".global " #name "_size\n" #name "_size:\n"                      \
            ".quad " #name "_end - " #name "\n");                            \
    extern const uint8_t name[];                                               \
    extern const unsigned long name##_size

INCASSET(ps5mc_tile_param_json, "assets/tile/param.json");
INCASSET(ps5mc_tile_icon_png, "assets/icon0.png");

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

static void installer_log(const char* format, ...) {
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

static int write_file(
    const char* path,
    const uint8_t* bytes,
    size_t size) {
    size_t offset = 0;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        return -errno;
    }
    while (offset < size) {
        const ssize_t written = write(descriptor, bytes + offset, size - offset);
        if (written <= 0) {
            const int error = written == 0 ? ENOSPC : errno;
            close(descriptor);
            return -error;
        }
        offset += (size_t)written;
    }
    if (close(descriptor) != 0) {
        return -errno;
    }
    return 0;
}

int main(void) {
    char sce_sys_dir[256];
    char param_path[320];
    char icon_path[320];
    uint32_t appinst_handle = 0;
    app_install_title_dir_fn install_title_dir = NULL;
    const pid_t pid = getpid();
    const uint64_t original_authid = kernel_get_ucred_authid(pid);
    int title_dir_rc = PS5MC_DIAG_SKIPPED;
    int install_all_rc = PS5MC_DIAG_SKIPPED;
    int final_rc = 1;

    installer_log(
        "start title=%s param_bytes=%lu icon_bytes=%lu",
        PS5MC_TITLE_ID,
        ps5mc_tile_param_json_size,
        ps5mc_tile_icon_png_size);
    if (ps5mc_tile_icon_png_size < 8 ||
        ps5mc_tile_icon_png[0] != 0x89 ||
        ps5mc_tile_icon_png[1] != 'P' ||
        ps5mc_tile_icon_png[2] != 'N' ||
        ps5mc_tile_icon_png[3] != 'G') {
        installer_log("reject invalid embedded icon");
        return 2;
    }

    (void)sceUserServiceInitialize(NULL);
    if (kernel_set_ucred_authid(pid, PS5MC_APPINST_AUTHID) != 0) {
        installer_log("failed to set AppInst authid");
        (void)sceUserServiceTerminate();
        return 1;
    }
    if (sceAppInstUtilInitialize() != 0) {
        installer_log("sceAppInstUtilInitialize failed");
        if (original_authid != 0) {
            (void)kernel_set_ucred_authid(pid, original_authid);
        }
        (void)sceUserServiceTerminate();
        return 1;
    }

    snprintf(sce_sys_dir, sizeof(sce_sys_dir), "%s/sce_sys", PS5MC_APP_DIR);
    snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys_dir);
    snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys_dir);
    if (mkdir_if_needed(PS5MC_APP_DIR) == 0 &&
        mkdir_if_needed(sce_sys_dir) == 0 &&
        write_file(
            param_path,
            ps5mc_tile_param_json,
            ps5mc_tile_param_json_size) == 0 &&
        write_file(
            icon_path,
            ps5mc_tile_icon_png,
            ps5mc_tile_icon_png_size) == 0) {
        const int handle_rc = kernel_dynlib_handle(
            -1,
            "libSceAppInstUtil.sprx",
            &appinst_handle);
        if (handle_rc == 0) {
            install_title_dir =
                (app_install_title_dir_fn)kernel_dynlib_resolve(
                    -1,
                    appinst_handle,
                    "Wudg3Xe3heE");
        }
        installer_log(
            "AppInstallTitleDir resolved=%s handle_rc=0x%08x",
            install_title_dir ? "yes" : "no",
            handle_rc);
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
            final_rc = 0;
        }
    }

    installer_log(
        "finish result=%s title_dir=0x%08x install_all=0x%08x",
        final_rc == 0 ? "installed" : "failed",
        title_dir_rc,
        install_all_rc);
    (void)sceAppInstUtilTerminate();
    if (original_authid != 0) {
        (void)kernel_set_ucred_authid(pid, original_authid);
    }
    (void)sceUserServiceTerminate();
    return final_rc;
}
