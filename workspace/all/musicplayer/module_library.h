#ifndef __MODULE_LIBRARY_H__
#define __MODULE_LIBRARY_H__

#include <SDL2/SDL.h>
#include "module_common.h"

// Run the library submenu (Files, Playlists, Downloader)
ModuleExitReason LibraryModule_run(SDL_Surface* screen);

// Set toast message (called by sub-modules returning to library with a message)
void LibraryModule_setToast(const char* message);

#endif
