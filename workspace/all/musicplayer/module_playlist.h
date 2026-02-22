#ifndef __MODULE_PLAYLIST_H__
#define __MODULE_PLAYLIST_H__

#include <SDL2/SDL.h>
#include "module_common.h"

// Run the playlist module (list → detail → playing)
ModuleExitReason PlaylistModule_run(SDL_Surface* screen);

#endif
