#ifndef UI_COMPONENTS_H
#define UI_COMPONENTS_H

#include "sdl.h"

void UI_renderConfirmDialog(SDL_Surface* dst, const char* title,
							const char* subtitle);

void UI_calcImageFit(int img_w, int img_h, int max_w, int max_h,
					 int* out_w, int* out_h);

SDL_Surface* UI_convertSurface(SDL_Surface* surface, SDL_Surface* screen);

void UI_renderCenteredMessage(SDL_Surface* dst, const char* message);

// Render the top menu bar: semi-transparent background, title text (left), hardware group (right).
// Returns the width of the hardware group (ow) for callers that need it.
int UI_renderMenuBar(SDL_Surface* screen, const char* title, int show_setting);

#endif // UI_COMPONENTS_H
