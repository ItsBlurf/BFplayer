#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BfplayerAudioConsumePlan {
    size_t offset;
    size_t bytes;
    size_t remaining;
} BfplayerAudioConsumePlan;

/*
 * Plans one aligned read from a decoded interleaved audio frame.
 *
 * frame_bytes is the original frame size and remaining_bytes is the amount
 * not yet copied. request_bytes is the free space in the audio backend queue.
 * The function never silently discards a partial frame.
 */
static inline bool bfplayer_audio_consume_plan(
    size_t frame_bytes,
    size_t remaining_bytes,
    size_t request_bytes,
    size_t sample_frame_bytes,
    BfplayerAudioConsumePlan *plan
) {
    if(!plan || sample_frame_bytes == 0 ||
       remaining_bytes > frame_bytes ||
       frame_bytes % sample_frame_bytes != 0 ||
       remaining_bytes % sample_frame_bytes != 0) {
        return false;
    }

    const size_t aligned_request =
        request_bytes - request_bytes % sample_frame_bytes;
    const size_t bytes =
        aligned_request < remaining_bytes
            ? aligned_request
            : remaining_bytes;

    plan->offset = frame_bytes - remaining_bytes;
    plan->bytes = bytes;
    plan->remaining = remaining_bytes - bytes;
    return true;
}

#ifdef __cplusplus
}
#endif
