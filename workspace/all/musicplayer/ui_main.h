#ifndef __UI_MAIN_H__
#define __UI_MAIN_H__

#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>

// Render the main menu (first_item_mode: 0=none, 1=Resume, 2=Now Playing)
void render_menu(SDL_Surface* screen, int show_setting, int menu_selected,
				 char* toast_message, uint32_t toast_time, int first_item_mode);

// Render controls help dialog overlay
void render_controls_help(SDL_Surface* screen, int app_state);

// Render screen off hint message
void render_screen_off_hint(SDL_Surface* screen);

// Check if Resume scroll needs continuous redraw (software scroll mode)
bool menu_needs_scroll_redraw(void);

#endif
