#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <ps5/payload.h>

#define BFPLAYER_LAUNCH_ADDRESS "127.0.0.1"
#define BFPLAYER_LAUNCH_PORT 9040
#define BFPLAYER_LAUNCH_LOG "/data/BFplayer/launch-helper.log"

static FILE *open_log(void) {
    FILE *log = fopen(BFPLAYER_LAUNCH_LOG, "w");
    return log ? log : stderr;
}

static int send_all(int descriptor, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t sent = send(descriptor, data + offset, size - offset, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return -1;
        }
        offset += (size_t)sent;
    }
    return 0;
}

int main(void) {
    static const char request[] =
        "GET /launch HTTP/1.1\r\n"
        "Host: 127.0.0.1:9040\r\n"
        "Connection: close\r\n"
        "\r\n";
    FILE *log = open_log();
    struct sockaddr_in address;
    struct timeval timeout;
    char response[512];
    int descriptor = -1;
    int result = 1;
    ssize_t received;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(BFPLAYER_LAUNCH_PORT);
    if (inet_pton(AF_INET, BFPLAYER_LAUNCH_ADDRESS, &address.sin_addr) != 1) {
        fprintf(log, "address-parse-failed\n");
        goto complete;
    }

    descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        fprintf(log, "socket-failed errno=%d\n", errno);
        goto complete;
    }

    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    (void)setsockopt(
        descriptor,
        SOL_SOCKET,
        SO_SNDTIMEO,
        &timeout,
        sizeof(timeout));
    (void)setsockopt(
        descriptor,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout));

    if (connect(
            descriptor,
            (const struct sockaddr *)&address,
            sizeof(address)) != 0) {
        fprintf(log, "connect-failed errno=%d\n", errno);
        goto complete;
    }
    if (send_all(descriptor, request, sizeof(request) - 1) != 0) {
        fprintf(log, "send-failed errno=%d\n", errno);
        goto complete;
    }

    received = recv(descriptor, response, sizeof(response) - 1, 0);
    if (received <= 0) {
        fprintf(log, "receive-failed errno=%d\n", errno);
        goto complete;
    }
    response[received] = '\0';
    if (strstr(response, "HTTP/1.1 200 ") == NULL &&
        strstr(response, "HTTP/1.0 200 ") == NULL) {
        fprintf(log, "unexpected-response bytes=%ld\n%s\n", (long)received, response);
        goto complete;
    }

    fprintf(log, "launch-request-success response_bytes=%ld\n", (long)received);
    result = 0;

complete:
    if (descriptor >= 0) {
        close(descriptor);
    }
    if (log != stderr) {
        fclose(log);
    }
    payload_exit(result);
    return result;
}
