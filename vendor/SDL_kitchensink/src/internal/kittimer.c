#include "kitchensink2/internal/kittimer.h"
#include "kitchensink2/internal/kittimerstate.h"
#include "kitchensink2/internal/utils/kithelpers.h"
#include <SDL_atomic.h>
#include <stdlib.h>

typedef struct Kit_TimerValue {
    int count;
    SDL_SpinLock lock;
    Kit_TimerState state;
} Kit_TimerValue;

struct Kit_Timer {
    bool writeable;
    Kit_TimerValue *ref;
};

Kit_Timer *Kit_CreateTimer() {
    Kit_Timer *timer;
    Kit_TimerValue *value;

    if((timer = calloc(1, sizeof(Kit_Timer))) == NULL) {
        goto exit_0;
    }
    if((value = calloc(1, sizeof(Kit_TimerValue))) == NULL) {
        goto exit_1;
    }

    value->count = 1;
    value->lock = 0;
    Kit_TimerStateReset(&value->state);
    timer->ref = value;
    timer->writeable = true;
    return timer;

exit_1:
    free(timer);
exit_0:
    return NULL;
}

Kit_Timer *Kit_CreateSecondaryTimer(const Kit_Timer *src, bool writeable) {
    Kit_Timer *timer;
    if((timer = calloc(1, sizeof(Kit_Timer))) == NULL) {
        return NULL;
    }
    timer->ref = src->ref;
    timer->ref->count++;
    timer->writeable = writeable;
    return timer;
}

void Kit_InitTimerBase(Kit_Timer *timer) {
    if(timer->writeable) {
        SDL_AtomicLock(&timer->ref->lock);
        Kit_TimerStateInit(&timer->ref->state, Kit_GetSystemTime());
        SDL_AtomicUnlock(&timer->ref->lock);
    }
}

bool Kit_IsTimerInitialized(const Kit_Timer *timer) {
    SDL_AtomicLock(&timer->ref->lock);
    const bool initialized = timer->ref->state.initialized;
    SDL_AtomicUnlock(&timer->ref->lock);
    return initialized;
}

void Kit_ResetTimerBase(Kit_Timer *timer) {
    if(timer->writeable) {
        SDL_AtomicLock(&timer->ref->lock);
        Kit_TimerStateReset(&timer->ref->state);
        SDL_AtomicUnlock(&timer->ref->lock);
    }
}

void Kit_SetTimerBase(Kit_Timer *timer) {
    if(timer->writeable) {
        SDL_AtomicLock(&timer->ref->lock);
        Kit_TimerStateSet(&timer->ref->state, Kit_GetSystemTime());
        SDL_AtomicUnlock(&timer->ref->lock);
    }
}

void Kit_AdjustTimerBase(Kit_Timer *timer, double adjust) {
    if(timer->writeable) {
        SDL_AtomicLock(&timer->ref->lock);
        Kit_TimerStateAdjust(
            &timer->ref->state,
            Kit_GetSystemTime(),
            adjust);
        SDL_AtomicUnlock(&timer->ref->lock);
    }
}

void Kit_AddTimerBase(Kit_Timer *timer, double add) {
    if(timer->writeable) {
        SDL_AtomicLock(&timer->ref->lock);
        Kit_TimerStateAdd(&timer->ref->state, add);
        SDL_AtomicUnlock(&timer->ref->lock);
    }
}

void Kit_PauseTimer(Kit_Timer *timer) {
    if(timer->writeable) {
        SDL_AtomicLock(&timer->ref->lock);
        Kit_TimerStatePause(
            &timer->ref->state,
            Kit_GetSystemTime());
        SDL_AtomicUnlock(&timer->ref->lock);
    }
}

void Kit_ResumeTimer(Kit_Timer *timer) {
    if(timer->writeable) {
        SDL_AtomicLock(&timer->ref->lock);
        Kit_TimerStateResume(
            &timer->ref->state,
            Kit_GetSystemTime());
        SDL_AtomicUnlock(&timer->ref->lock);
    }
}

double Kit_GetTimerElapsed(const Kit_Timer *timer) {
    SDL_AtomicLock(&timer->ref->lock);
    const double elapsed = Kit_TimerStateElapsed(
        &timer->ref->state,
        Kit_GetSystemTime());
    SDL_AtomicUnlock(&timer->ref->lock);
    return elapsed;
}

bool Kit_IsTimerPrimary(const Kit_Timer *timer) {
    return timer->writeable;
}

void Kit_CloseTimer(Kit_Timer **ref) {
    if(!ref || !*ref)
        return;
    Kit_Timer *timer = *ref;
    if(--timer->ref->count == 0) {
        free(timer->ref);
    }
    free(timer);
    *ref = NULL;
}
