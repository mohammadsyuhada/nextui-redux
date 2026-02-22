#ifndef __UI_PODCAST_H__
#define __UI_PODCAST_H__

#include <SDL2/SDL.h>
#include <stdbool.h>

// Podcast manage menu items (Y button menu)
typedef enum {
	PODCAST_MANAGE_SEARCH = 0,
	PODCAST_MANAGE_TOP_SHOWS,
	PODCAST_MANAGE_COUNT
} PodcastManageMenuItem;

// Render redesigned podcast main page (continue listening + subscriptions)
void render_podcast_main_page(SDL_Surface* screen, int show_setting,
							  int selected, int* scroll,
							  const char* toast_message, uint32_t toast_time);

// Clear thumbnail cache (call from Podcast_cleanup)
void Podcast_clearThumbnailCache(void);

// Lazy load one pending thumbnail from disk (call from main loop)
// Returns true if a thumbnail was loaded (caller should set dirty)
bool Podcast_loadPendingThumbnails(void);

// Render the podcast management menu (Y button opens this)
void render_podcast_manage(SDL_Surface* screen, int show_setting,
						   int menu_selected, int subscription_count);

// Render Top Shows list
void render_podcast_top_shows(SDL_Surface* screen, int show_setting,
							  int selected, int* scroll,
							  const char* toast_message, uint32_t toast_time);

// Render search results
void render_podcast_search_results(SDL_Surface* screen, int show_setting,
								   int selected, int* scroll,
								   const char* toast_message, uint32_t toast_time);

// Render episode list for a feed
void render_podcast_episodes(SDL_Surface* screen, int show_setting,
							 int feed_index, int selected, int* scroll,
							 const char* toast_message, uint32_t toast_time);

// Render download queue view
void render_podcast_download_queue(SDL_Surface* screen, int show_setting,
								   int selected, int* scroll,
								   const char* toast_message, uint32_t toast_time);

// Render now playing screen for podcast
void render_podcast_playing(SDL_Surface* screen, int show_setting,
							int feed_index, int episode_index);

// Render loading screen (for fetching feed, charts, etc.)
void render_podcast_loading(SDL_Surface* screen, const char* message);

// Check if podcast title is currently scrolling (for refresh)
bool Podcast_isTitleScrolling(void);

// Check if title scroll needs a render to transition (delay phase)
bool Podcast_titleScrollNeedsRender(void);

// Animate podcast title scroll only (GPU mode, no screen redraw needed)
void Podcast_animateTitleScroll(void);

// Clear podcast title scroll state (call when selection changes)
void Podcast_clearTitleScroll(void);

// Clear podcast artwork and playing title scroll (call when leaving playing screen)
void Podcast_clearArtwork(void);

// === PODCAST PROGRESS GPU FUNCTIONS ===
// GPU layer for podcast progress (uses LAYER_PLAYTIME since music player isn't active)
#define LAYER_PODCAST_PROGRESS 3

// Set position for GPU rendering (call once during initial render)
void PodcastProgress_setPosition(int bar_x, int bar_y, int bar_w, int bar_h,
								 int time_y, int screen_w, int duration_ms);

// Clear progress state (call when leaving playing screen)
void PodcastProgress_clear(void);

// Check if progress needs refresh (position changed by 1 second)
bool PodcastProgress_needsRefresh(void);

// Render progress bar and time to GPU layer (call from main loop)
void PodcastProgress_renderGPU(void);

#endif
