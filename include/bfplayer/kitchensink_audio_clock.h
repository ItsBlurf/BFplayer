#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns the media timestamp currently reaching the audio device.
 *
 * next_sample_pts is the timestamp of the next byte that will be appended to
 * the backend queue. backend_buffer_bytes is the amount already waiting in
 * that queue. NAN is returned when the format or timestamp is invalid.
 */
double bfplayer_audio_audible_position(
    double next_sample_pts,
    size_t backend_buffer_bytes,
    int sample_rate,
    int channels,
    int bytes_per_sample);

#ifdef __cplusplus
}
#endif
