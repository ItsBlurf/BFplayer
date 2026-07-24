#pragma once

#include <SDL.h>

namespace ps5mc {

// PacBrew's PS5 SDL backend exposes the physical DualSense buttons through
// legacy game-controller slots:
//   PS5 Options  -> SDL_CONTROLLER_BUTTON_BACK
//   PS5 Touchpad -> SDL_CONTROLLER_BUTTON_START
// Generic desktop SDL uses the semantic START and TOUCHPAD slots instead.
#if defined(PS5MC_PS5)
inline constexpr SDL_GameControllerButton kControllerOptionsButton =
    SDL_CONTROLLER_BUTTON_BACK;
inline constexpr SDL_GameControllerButton kControllerTouchpadButton =
    SDL_CONTROLLER_BUTTON_START;
#else
inline constexpr SDL_GameControllerButton kControllerOptionsButton =
    SDL_CONTROLLER_BUTTON_START;
inline constexpr SDL_GameControllerButton kControllerTouchpadButton =
    SDL_CONTROLLER_BUTTON_TOUCHPAD;
#endif

} // namespace ps5mc
