#ifndef UI_LIST_H
#define UI_LIST_H

#include <stdbool.h>
#include <stdint.h>
#include "defines.h"
#include "api.h"

// ---- Scroll Text (marquee animation) ----

typedef struct {
	char text[512];
	int text_width;
	int max_width;
	uint32_t start_time;
	bool needs_scroll;
	int scroll_offset;
	bool use_gpu_scroll;
	int last_x, last_y;
	TTF_Font* last_font;
	SDL_Color last_color;
	SDL_Surface* cached_scroll_surface;
	bool scroll_active;
} ScrollTextState;

void ScrollText_reset(ScrollTextState* state, const char* text,
					  TTF_Font* font, int max_width, bool use_gpu);
bool ScrollText_isScrolling(ScrollTextState* state);
bool ScrollText_needsRender(ScrollTextState* state);
void ScrollText_activateAfterDelay(ScrollTextState* state);
void ScrollText_animateOnly(ScrollTextState* state);
void ScrollText_render(ScrollTextState* state, TTF_Font* font,
					   SDL_Color color, SDL_Surface* screen, int x, int y);
void ScrollText_update(ScrollTextState* state, const char* text,
					   TTF_Font* font, int max_width, SDL_Color color,
					   SDL_Surface* screen, int x, int y, bool use_gpu);

// ---- List Layout ----

typedef struct {
	int list_y;			// Y where list starts
	int list_h;			// Height available for list
	int item_h;			// Height per item
	int items_per_page; // Visible item count
	int max_width;		// Max content width
} ListLayout;

ListLayout UI_calcListLayout(SDL_Surface* screen);

// ---- Pill Rendering (stateless) ----

typedef struct {
	int pill_width;
	int text_x;
	int text_y;
} ListItemPos;

int UI_calcListPillWidth(TTF_Font* font, const char* text, char* truncated,
						 int max_width, int prefix_width);
void UI_drawListItemBg(SDL_Surface* dst, SDL_Rect* rect, bool selected);
SDL_Color UI_getListTextColor(bool selected);

ListItemPos UI_renderListItemPill(SDL_Surface* screen, ListLayout* layout,
								  TTF_Font* font, const char* text,
								  char* truncated, int y, bool selected,
								  int prefix_width);

void UI_renderListItemText(SDL_Surface* screen, ScrollTextState* scroll_state,
						   const char* text, TTF_Font* font,
						   int text_x, int text_y, int max_text_width,
						   bool selected);

// ---- Badged Pill Rendering ----

typedef struct {
	int pill_width;		// Width of the title (inner) pill
	int text_x;			// X position for title text
	int text_y;			// Y position for title text (row 1)
	int subtitle_x;		// X position for subtitle text (row 2)
	int subtitle_y;		// Y position for subtitle text (row 2)
	int badge_x;		// X position for badge content start
	int badge_y;		// Y position for badge content (centered)
	int total_width;	// Total width of title pill + badge area
	int text_max_width; // Max width for text content
} ListItemBadgedPos;

// Render a two-row list item pill with optional right-side badge area.
// Item height is 1.5x PILL_SIZE. Title (title_font) + subtitle (subtitle_font).
// When badge_width > 0 and selected: THEME_COLOR2 outer capsule + THEME_COLOR1 inner.
// When badge_width == 0: single THEME_COLOR1 capsule.
// Caller renders badge content at badge_x, badge_y.
ListItemBadgedPos UI_renderListItemPillBadged(
	SDL_Surface* screen, ListLayout* layout,
	TTF_Font* title_font, TTF_Font* subtitle_font, TTF_Font* badge_font,
	const char* text, const char* subtitle, char* truncated,
	int y, bool selected, int badge_width, int extra_subtitle_width);

// ---- Scroll Helpers ----

void UI_adjustListScroll(int selected, int* scroll, int items_per_page);
void UI_renderScrollIndicators(SDL_Surface* screen, int scroll,
							   int items_per_page, int total_count);

// ---- Pill Animation (non-threaded, for main-loop driven apps) ----

typedef struct {
	int current_y;
	int target_y;
	int start_y;
	int frame;
	int total_frames;
	bool active;
} PillAnimState;

void UI_pillAnimInit(PillAnimState* state);
void UI_pillAnimSetTarget(PillAnimState* state, int target_y, bool animate);
int UI_pillAnimTick(PillAnimState* state);
bool UI_pillAnimIsActive(PillAnimState* state);

#endif // UI_LIST_H
