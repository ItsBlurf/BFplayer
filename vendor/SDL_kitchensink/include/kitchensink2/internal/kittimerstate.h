#pragma once

#include <stdbool.h>

typedef struct Kit_TimerState {
    bool initialized;
    bool paused;
    double base;
    double paused_elapsed;
} Kit_TimerState;

static inline void Kit_TimerStateReset(Kit_TimerState *state) {
    state->initialized = false;
    state->paused = false;
    state->base = 0.0;
    state->paused_elapsed = 0.0;
}

static inline void Kit_TimerStateInit(Kit_TimerState *state, double now) {
    if(!state->initialized) {
        state->initialized = true;
        state->paused = false;
        state->base = now;
        state->paused_elapsed = 0.0;
    }
}

static inline void Kit_TimerStateSet(Kit_TimerState *state, double now) {
    state->initialized = true;
    state->paused = false;
    state->base = now;
    state->paused_elapsed = 0.0;
}

static inline void Kit_TimerStateAdjust(
    Kit_TimerState *state,
    double now,
    double elapsed
) {
    state->initialized = true;
    state->base = now - elapsed;
    if(state->paused)
        state->paused_elapsed = elapsed;
}

static inline void Kit_TimerStateAdd(
    Kit_TimerState *state,
    double amount
) {
    state->base += amount;
    if(state->paused)
        state->paused_elapsed -= amount;
    state->initialized = true;
}

static inline void Kit_TimerStatePause(
    Kit_TimerState *state,
    double now
) {
    if(state->initialized && !state->paused) {
        state->paused_elapsed = now - state->base;
        state->paused = true;
    }
}

static inline void Kit_TimerStateResume(
    Kit_TimerState *state,
    double now
) {
    if(state->initialized && state->paused) {
        state->base = now - state->paused_elapsed;
        state->paused = false;
    }
}

static inline double Kit_TimerStateElapsed(
    const Kit_TimerState *state,
    double now
) {
    if(!state->initialized)
        return 0.0;
    return state->paused
        ? state->paused_elapsed
        : now - state->base;
}
