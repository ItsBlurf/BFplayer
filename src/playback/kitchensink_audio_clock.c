#include "bfplayer/kitchensink_audio_clock.h"

#include <math.h>
#include <stdint.h>

double bfplayer_audio_audible_position(
    double next_sample_pts,
    size_t backend_buffer_bytes,
    int sample_rate,
    int channels,
    int bytes_per_sample) {
    if (!__builtin_isfinite(next_sample_pts) || sample_rate <= 0 || channels <= 0 ||
        bytes_per_sample <= 0) {
        return NAN;
    }

    const uint64_t frame_bytes =
        (uint64_t)(unsigned int)channels *
        (uint64_t)(unsigned int)bytes_per_sample;
    if (frame_bytes == 0 ||
        frame_bytes >
            UINT64_MAX / (uint64_t)(unsigned int)sample_rate) {
        return NAN;
    }
    const uint64_t bytes_per_second =
        (uint64_t)(unsigned int)sample_rate * frame_bytes;
    if (bytes_per_second == 0 ||
        bytes_per_second > (uint64_t)SIZE_MAX) {
        return NAN;
    }
    const uint64_t max_queue =
        bytes_per_second > UINT64_MAX / 10U
            ? UINT64_MAX
            : bytes_per_second * 10U;
    if ((uint64_t)backend_buffer_bytes > max_queue) {
        return NAN;
    }

    const double queued_seconds =
        (double)backend_buffer_bytes / (double)bytes_per_second;
    const double audible = next_sample_pts - queued_seconds;
    return audible > 0.0 ? audible : 0.0;
}
