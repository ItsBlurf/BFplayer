#pragma once

#include <SDL_render.h>

typedef struct Kit_Player Kit_Player;

#ifdef __cplusplus
extern "C" {
#endif

// SDL_kitchensink 2.0.0-a2 already accepts an explicit synchronization
// timestamp in its internal subtitle decoder, but its public player wrapper
// always supplies the unadjusted player clock. This narrow adapter preserves
// the installed ABI and exposes that existing timestamp input to the app.
int bfplayer_get_player_subtitle_texture_at(
    const Kit_Player* player,
    SDL_Texture* texture,
    SDL_Rect* sources,
    SDL_Rect* targets,
    int limit,
    double sync_seconds);

#ifdef __cplusplus
}
#endif
