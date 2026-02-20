#ifndef UI_CONNECT_H
#define UI_CONNECT_H

#include "sdl.h"
#include <stdbool.h>

typedef enum {
	CONNECT_NONE,	// still active, keep calling
	CONNECT_DONE,	// finished (connected or dismissed)
	CONNECT_CANCEL, // user backed out
} ConnectAction;

typedef struct {
	ConnectAction action;
	bool dirty;
} ConnectResult;

void ConnectDialog_initWifi(void);
void ConnectDialog_initBluetooth(void);
ConnectResult ConnectDialog_handleInput(void);
void ConnectDialog_render(SDL_Surface* screen);
void ConnectDialog_quit(void);

#endif // UI_CONNECT_H
