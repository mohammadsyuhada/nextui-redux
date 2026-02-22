#ifndef __MODULE_DOWNLOADER_H__
#define __MODULE_DOWNLOADER_H__

#include <SDL2/SDL.h>
#include "module_common.h"

// Run the downloader (YouTube) module
// Handles: Search, results, queue, downloading, yt-dlp updates
ModuleExitReason DownloaderModule_run(SDL_Surface* screen);

#endif
