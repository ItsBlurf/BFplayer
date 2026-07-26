/*
 * SDL_kitchensink 2.0.0-a2 interoperability adapter.
 * SDL_kitchensink is Copyright (c) 2018 Tuomas Virtanen, MIT licensed.
 */

#include "ps5mc/kitchensink_subtitle_timing.h"

#include <string.h>

typedef struct Kit_Decoder Kit_Decoder;

enum {
    PS5MC_KIT_STOPPED = 0
};

void Kit_GetSubtitleDecoderSDLTexture(
    const Kit_Decoder* decoder,
    SDL_Texture* texture,
    double sync_seconds);
int Kit_GetSubtitleDecoderSDLTextureInfo(
    const Kit_Decoder* decoder,
    SDL_Rect* sources,
    SDL_Rect* targets,
    int limit);

typedef struct Ps5mcKitPlayerPrefix {
    int state;
    Kit_Decoder* decoders[3];
} Ps5mcKitPlayerPrefix;

int ps5mc_get_player_subtitle_texture_at(
    const Kit_Player* player,
    SDL_Texture* texture,
    SDL_Rect* sources,
    SDL_Rect* targets,
    int limit,
    double sync_seconds) {
    if (!player || !texture || !sources || !targets || limit < 0) {
        return -1;
    }

    Ps5mcKitPlayerPrefix prefix;
    memset(&prefix, 0, sizeof(prefix));
    memcpy(&prefix, player, sizeof(prefix));
    Kit_Decoder* const subtitle_decoder = prefix.decoders[2];
    if (!subtitle_decoder || prefix.state == PS5MC_KIT_STOPPED) {
        return 0;
    }

    Kit_GetSubtitleDecoderSDLTexture(
        subtitle_decoder,
        texture,
        sync_seconds < 0.0 ? 0.0 : sync_seconds);
    return Kit_GetSubtitleDecoderSDLTextureInfo(
        subtitle_decoder,
        sources,
        targets,
        limit);
}
