#pragma once

#include <stdbool.h>
#include <float.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BfplayerAudioConsumePlan {
    size_t offset;
    size_t bytes;
    size_t remaining;
} BfplayerAudioConsumePlan;

typedef struct BfplayerAudioSeekPlan {
    size_t skip_bytes;
    bool target_reached;
} BfplayerAudioSeekPlan;

/*
 * Plans how much stale decoded audio to discard after a seek. The result is
 * sample-frame aligned and can span multiple decoded frames without emitting
 * audio from before the requested timestamp.
 */
static inline bool bfplayer_audio_seek_plan(
    size_t frame_bytes,
    size_t remaining_bytes,
    size_t sample_frame_bytes,
    double frame_start_pts,
    double bytes_per_second,
    double target_pts,
    BfplayerAudioSeekPlan *plan
) {
    if(!plan || sample_frame_bytes == 0 ||
       remaining_bytes > frame_bytes ||
       frame_bytes % sample_frame_bytes != 0 ||
       remaining_bytes % sample_frame_bytes != 0 ||
       frame_start_pts != frame_start_pts ||
       frame_start_pts > DBL_MAX || frame_start_pts < -DBL_MAX ||
       target_pts != target_pts ||
       target_pts > DBL_MAX || target_pts < -DBL_MAX ||
       bytes_per_second <= 0.0 ||
       bytes_per_second > DBL_MAX) {
        return false;
    }

    const size_t consumed_bytes = frame_bytes - remaining_bytes;
    const double current_pts =
        frame_start_pts + (double)consumed_bytes / bytes_per_second;
    if(target_pts <= current_pts) {
        plan->skip_bytes = 0;
        plan->target_reached = true;
        return true;
    }

    const double seconds_to_target = target_pts - current_pts;
    const double remaining_seconds =
        (double)remaining_bytes / bytes_per_second;
    if(seconds_to_target >= remaining_seconds) {
        plan->skip_bytes = remaining_bytes;
        plan->target_reached = false;
        return true;
    }

    const double sample_frames_to_target =
        seconds_to_target * bytes_per_second /
        (double)sample_frame_bytes;
    const size_t available_sample_frames =
        remaining_bytes / sample_frame_bytes;
    if(sample_frames_to_target >= (double)available_sample_frames) {
        plan->skip_bytes = remaining_bytes;
        plan->target_reached = false;
        return true;
    }
    size_t sample_frames = (size_t)sample_frames_to_target;
    if((double)sample_frames < sample_frames_to_target)
        sample_frames++;
    if(sample_frames > available_sample_frames) {
        plan->skip_bytes = remaining_bytes;
        plan->target_reached = false;
        return true;
    }
    const size_t skip_bytes = sample_frames * sample_frame_bytes;
    if(skip_bytes >= remaining_bytes) {
        plan->skip_bytes = remaining_bytes;
        plan->target_reached = false;
        return true;
    }
    plan->skip_bytes = skip_bytes;
    plan->target_reached = true;
    return true;
}

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
