#ifndef __UI_FONTS_H__
#define __UI_FONTS_H__

#include <SDL2/SDL_ttf.h>
#include <stdbool.h>

// Theme color helpers for list items (follows system appearance)
SDL_Color Fonts_getListTextColor(bool selected);
void Fonts_drawListItemBg(SDL_Surface* screen, SDL_Rect* rect, bool selected);

// Calculate pill width for list items
// prefix_width: width of any prefix elements (indicator, checkbox, status) - pass 0 for simple text
// Returns the calculated pill width and fills truncated buffer with the truncated text
int Fonts_calcListPillWidth(TTF_Font* font, const char* text, char* truncated, int max_width, int prefix_width);

#endif
