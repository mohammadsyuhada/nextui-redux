#ifndef __UI_DOWNLOADER_H__
#define __UI_DOWNLOADER_H__

#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include "downloader.h"

// Render downloader sub-menu
void render_downloader_menu(SDL_Surface* screen, int show_setting, int menu_selected,
							char* toast_message, uint32_t toast_time);

// Render downloader searching status
void render_downloader_searching(SDL_Surface* screen, int show_setting, const char* search_query);

// Render downloader search results
void render_downloader_results(SDL_Surface* screen, int show_setting,
							   const char* search_query,
							   DownloaderResult* results, int result_count,
							   int selected, int* scroll,
							   char* toast_message, uint32_t toast_time, bool searching);

// Render downloader download queue
void render_downloader_queue(SDL_Surface* screen, int show_setting,
							 int queue_selected, int* queue_scroll);

// Check if downloader results list has active scrolling (for refresh optimization)
bool downloader_results_needs_scroll_refresh(void);

// Check if results scroll needs a render to transition (delay phase)
bool downloader_results_scroll_needs_render(void);

// Check if downloader queue list has active scrolling (for refresh optimization)
bool downloader_queue_needs_scroll_refresh(void);

// Check if queue scroll needs a render to transition (delay phase)
bool downloader_queue_scroll_needs_render(void);

// Animate downloader results scroll only (GPU mode, no screen redraw needed)
void downloader_results_animate_scroll(void);

// Animate downloader queue scroll only (GPU mode, no screen redraw needed)
void downloader_queue_animate_scroll(void);

// Clear downloader queue scroll state (call when queue items are removed)
void downloader_queue_clear_scroll(void);

// Clear downloader results scroll state and toast (call when leaving results screen)
void downloader_results_clear_scroll(void);

#endif
