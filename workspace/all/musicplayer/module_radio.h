#ifndef __MODULE_RADIO_H__
#define __MODULE_RADIO_H__

#include <SDL2/SDL.h>
#include "module_common.h"

// Run the online radio module
// Handles: Station list, playback, adding stations from curated list
ModuleExitReason RadioModule_run(SDL_Surface* screen);

#endif
