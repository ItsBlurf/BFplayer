/* SPDX-License-Identifier: MIT
 * BFplayer extension for bounded SDL_kitchensink packet queues.
 */
#ifndef KITPACKETBUDGET_H
#define KITPACKETBUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KIT_VIDEO_PACKET_BYTE_LIMIT (96U * 1024U * 1024U)
#define KIT_AUDIO_PACKET_BYTE_LIMIT (16U * 1024U * 1024U)
#define KIT_SUBTITLE_PACKET_BYTE_LIMIT (8U * 1024U * 1024U)

static inline bool Kit_PacketBudgetAllows(
    size_t used,
    size_t limit,
    size_t incoming,
    bool empty
) {
    if(limit == 0 || empty)
        return true;
    if(used >= limit)
        return false;
    return incoming <= limit - used;
}

static inline size_t Kit_PacketBudgetAdd(
    size_t used,
    size_t incoming
) {
    return incoming > SIZE_MAX - used
        ? SIZE_MAX
        : used + incoming;
}

static inline size_t Kit_PacketBudgetRemove(
    size_t used,
    size_t removed
) {
    return removed >= used ? 0 : used - removed;
}

#endif // KITPACKETBUDGET_H
