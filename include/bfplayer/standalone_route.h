#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Accept exactly the launch path in an HTTP GET request line.
 * Query strings, alternate methods, and prefix/suffix paths are rejected.
 */
int bfplayer_request_is_launch(const char* request);
int bfplayer_request_is_shutdown(const char* request);

#ifdef __cplusplus
}
#endif
