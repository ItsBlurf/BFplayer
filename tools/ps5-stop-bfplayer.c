#include <stdio.h>
#include <string.h>

#include <ps5/payload.h>

int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceGetAppTitleId(int app_id, char *title_id);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);

static FILE *open_log(void) {
    FILE *log = fopen("/data/BFplayer/stop-helper.log", "w");
    return log ? log : stderr;
}

int main(void) {
    static const char standalone_title_id[] = "PSMC00001";
    static const char websrv_title_id[] = "FAKE00000";
    FILE *log = open_log();
    char title_id[16] = {0};
    const int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0) {
        fprintf(log, "no-running-big-app app_id=%d\n", app_id);
        if (log != stderr) {
            fclose(log);
        }
        payload_exit(1);
        return 1;
    }
    const int title_result =
        sceSystemServiceGetAppTitleId(app_id, title_id);
    if (title_result != 0) {
        fprintf(
            log,
            "title-lookup-failed app_id=%d result=0x%08x\n",
            app_id,
            (unsigned int)title_result);
        if (log != stderr) {
            fclose(log);
        }
        payload_exit(1);
        return 1;
    }
    if (strcmp(title_id, standalone_title_id) != 0 &&
        strcmp(title_id, websrv_title_id) != 0) {
        fprintf(
            log,
            "refusing-unexpected-app app_id=%d title_id=%s\n",
            app_id,
            title_id);
        if (log != stderr) {
            fclose(log);
        }
        payload_exit(1);
        return 1;
    }
    fprintf(
        log,
        "kill-request app_id=%d title_id=%s\n",
        app_id,
        title_id);
    fflush(log);
    const int result = sceSystemServiceKillApp(app_id, -1, 0, 0);
    if (result != 0) {
        fprintf(
            log,
            "kill-failed result=0x%08x\n",
            (unsigned int)result);
        if (log != stderr) {
            fclose(log);
        }
        payload_exit(1);
        return 1;
    }
    fprintf(log, "kill-success\n");
    if (log != stderr) {
        fclose(log);
    }
    payload_exit(0);
    return 0;
}
