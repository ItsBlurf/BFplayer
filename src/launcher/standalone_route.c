#include "ps5mc/standalone_route.h"

#include <string.h>

int ps5mc_request_is_launch(const char* request) {
    static const char prefix[] = "GET /launch ";
    return request &&
        strncmp(request, prefix, sizeof(prefix) - 1U) == 0;
}

int ps5mc_request_is_shutdown(const char* request) {
    static const char prefix[] = "GET /shutdown ";
    return request &&
        strncmp(request, prefix, sizeof(prefix) - 1U) == 0;
}
