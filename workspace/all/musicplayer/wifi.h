#ifndef __WIFI_H__
#define __WIFI_H__

#include <SDL2/SDL.h>
#include <stdbool.h>

// Ensure WiFi is connected, enabling if necessary
// Returns true if connected, false otherwise
// Shows "Connecting..." screen while waiting (if scr is not NULL)
// Can be called from background threads with scr=NULL to skip UI rendering
bool Wifi_ensureConnected(SDL_Surface* scr, int show_setting);

// Check if WiFi is currently connected
bool Wifi_isConnected(void);

#endif
