# BFplayer PS5 SDL video override

BFplayer links one patched object from the PS5 SDL backend before the PacBrew
`libSDL2.a`. The override is based on:

- Repository: `https://github.com/ps5-payload-dev/SDL.git`
- Commit: `0baf4ac49382b537ba449901b5b6d0d189bb1fbb`
- SDL revision: `SDL-2.30.12-g0baf4ac49`

The patch exposes the backend's existing 3840x2160 display mode, recreates the
software framebuffer when the mode changes, scales a mismatched window surface
correctly, validates that each framebuffer fits in the backend's allocated
direct-memory slot, and uses a persistent parallel tile pool with pipelined
framebuffer flips to reduce 4K presentation cost.

Run `tools/rebuild-sdl-ps5-video.ps1` from the repository root to reproduce
`lib/SDL_ps5video.c.o`. The script requires the matching SDL source checkout
and the PS5 build environment.
