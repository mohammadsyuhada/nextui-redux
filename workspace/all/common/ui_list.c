#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ui_list.h"

// Scroll gap for software scrolling
#define SCROLL_GAP 30

// Delay before scrolling starts (ms) - show static text first
#define SCROLL_START_DELAY 1000

// ============================================
// Scroll Text (marquee animation)
// ============================================

void ScrollText_reset(ScrollTextState* state, const char* text, TTF_Font* font, int max_width, bool use_gpu) {
	GFX_clearLayers(LAYER_SCROLLTEXT);

	if (state->cached_scroll_surface) {
		SDL_FreeSurface(state->cached_scroll_surface);
		state->cached_scroll_surface = NULL;
	}

	strncpy(state->text, text, sizeof(state->text) - 1);
	state->text[sizeof(state->text) - 1] = '\0';
	int text_h = 0;
	TTF_SizeUTF8(font, state->text, &state->text_width, &text_h);
	state->max_width = max_width;
	state->start_time = SDL_GetTicks();
	state->scroll_offset = 0;
	state->use_gpu_scroll = use_gpu;
	state->scroll_active = false;
	state->needs_scroll = false;

	if ((state->text_width > max_width) && use_gpu) {
		int padding = SCALE1(SCROLL_GAP);
		int total_width = state->text_width * 2 + padding;
		int height = TTF_FontHeight(font);

		state->cached_scroll_surface = SDL_CreateRGBSurfaceWithFormat(0,
																	  total_width, height, 32, SDL_PIXELFORMAT_ARGB8888);

		if (state->cached_scroll_surface) {
			SDL_FillRect(state->cached_scroll_surface, NULL, 0);

			SDL_Color white = {255, 255, 255, 255};
			SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font, state->text, white);
			if (text_surf) {
				SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_NONE);
				SDL_BlitSurface(text_surf, NULL, state->cached_scroll_surface, &(SDL_Rect){0, 0, 0, 0});
				SDL_BlitSurface(text_surf, NULL, state->cached_scroll_surface, &(SDL_Rect){state->text_width + padding, 0, 0, 0});
				SDL_FreeSurface(text_surf);
			}
		}
	}
}

bool ScrollText_isScrolling(ScrollTextState* state) {
	return state->needs_scroll;
}

bool ScrollText_needsRender(ScrollTextState* state) {
	return state->text[0] && state->text_width > state->max_width && !state->needs_scroll;
}

void ScrollText_activateAfterDelay(ScrollTextState* state) {
	if (!state->needs_scroll && state->text_width > state->max_width &&
		SDL_GetTicks() - state->start_time >= SCROLL_START_DELAY) {
		state->needs_scroll = true;
	}
}

void ScrollText_animateOnly(ScrollTextState* state) {
	if (!state->text[0] || !state->needs_scroll || !state->use_gpu_scroll)
		return;
	if (!state->last_font)
		return;

	GFX_clearLayers(LAYER_SCROLLTEXT);
	GFX_scrollTextTexture(
		state->last_font,
		state->text,
		state->last_x, state->last_y,
		state->max_width,
		TTF_FontHeight(state->last_font),
		state->last_color,
		1.0f,
		NULL);
}

void ScrollText_render(ScrollTextState* state, TTF_Font* font, SDL_Color color,
					   SDL_Surface* screen, int x, int y) {
	if (!state->text[0])
		return;

	state->last_x = x;
	state->last_y = y;
	state->last_font = font;
	state->last_color = color;

	if (!state->needs_scroll && state->text_width > state->max_width &&
		SDL_GetTicks() - state->start_time >= SCROLL_START_DELAY) {
		if (state->use_gpu_scroll && !state->scroll_active) {
			GFX_resetScrollText();
			state->scroll_active = true;
		} else {
			state->needs_scroll = true;
		}
	}

	if (!state->needs_scroll) {
		GFX_clearLayers(LAYER_SCROLLTEXT);
		SDL_Surface* surf = TTF_RenderUTF8_Blended(font, state->text, color);
		if (surf) {
			SDL_Rect src = {0, 0, surf->w > state->max_width ? state->max_width : surf->w, surf->h};
			SDL_BlitSurface(surf, &src, screen, &(SDL_Rect){x, y, 0, 0});
			SDL_FreeSurface(surf);
		}
		return;
	}

	if (state->use_gpu_scroll) {
		GFX_clearLayers(LAYER_SCROLLTEXT);
		GFX_scrollTextTexture(
			font,
			state->text,
			x, y,
			state->max_width,
			TTF_FontHeight(font),
			color,
			1.0f,
			NULL);
	} else {
		GFX_clearLayers(LAYER_SCROLLTEXT);

		SDL_Surface* single_surf = TTF_RenderUTF8_Blended(font, state->text, color);
		if (!single_surf)
			return;

		SDL_Surface* full_surf = SDL_CreateRGBSurfaceWithFormat(0,
																state->text_width * 2 + SCROLL_GAP, single_surf->h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!full_surf) {
			SDL_FreeSurface(single_surf);
			return;
		}

		SDL_FillRect(full_surf, NULL, 0);
		SDL_SetSurfaceBlendMode(single_surf, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(single_surf, NULL, full_surf, &(SDL_Rect){0, 0, 0, 0});
		SDL_BlitSurface(single_surf, NULL, full_surf, &(SDL_Rect){state->text_width + SCROLL_GAP, 0, 0, 0});
		SDL_FreeSurface(single_surf);

		state->scroll_offset += 2;
		if (state->scroll_offset >= state->text_width + SCROLL_GAP) {
			state->scroll_offset = 0;
		}

		SDL_SetSurfaceBlendMode(full_surf, SDL_BLENDMODE_BLEND);
		SDL_Rect src = {state->scroll_offset, 0, state->max_width, full_surf->h};
		SDL_Rect dst = {x, y, 0, 0};
		SDL_BlitSurface(full_surf, &src, screen, &dst);
		SDL_FreeSurface(full_surf);
	}
}

void ScrollText_update(ScrollTextState* state, const char* text, TTF_Font* font,
					   int max_width, SDL_Color color, SDL_Surface* screen, int x, int y, bool use_gpu) {
	if (strcmp(state->text, text) != 0) {
		ScrollText_reset(state, text, font, max_width, use_gpu);
	}
	ScrollText_render(state, font, color, screen, x, y);
}

// ============================================
// List Layout
// ============================================

ListLayout UI_calcListLayout(SDL_Surface* screen) {
	int hw = screen->w;
	int hh = screen->h;

	ListLayout layout;
	layout.list_y = SCALE1(PADDING + PILL_SIZE) + 10;
	layout.list_h = hh - layout.list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
	layout.item_h = SCALE1(PILL_SIZE);
	layout.items_per_page = layout.list_h / layout.item_h;
	layout.max_width = hw - SCALE1(PADDING * 2);

	return layout;
}

// ============================================
// Pill Rendering (stateless)
// ============================================

int UI_calcListPillWidth(TTF_Font* font, const char* text, char* truncated, int max_width, int prefix_width) {
	int available_width = max_width - prefix_width;
	int padding = SCALE1(BUTTON_PADDING * 2);

	int raw_text_w, raw_text_h;
	TTF_SizeUTF8(font, text, &raw_text_w, &raw_text_h);

	if (raw_text_w + padding > available_width) {
		GFX_truncateText(font, text, truncated, available_width, padding);
		return max_width;
	}

	strncpy(truncated, text, 255);
	truncated[255] = '\0';
	return MIN(max_width, prefix_width + raw_text_w + padding);
}

void UI_drawListItemBg(SDL_Surface* dst, SDL_Rect* rect, bool selected) {
	if (selected) {
		GFX_blitPillColor(ASSET_WHITE_PILL, dst, rect, THEME_COLOR1, RGB_WHITE);
	}
}

SDL_Color UI_getListTextColor(bool selected) {
	return selected ? uintToColour(THEME_COLOR5_255) : uintToColour(THEME_COLOR4_255);
}

ListItemPos UI_renderListItemPill(SDL_Surface* screen, ListLayout* layout,
								  TTF_Font* font, const char* text,
								  char* truncated, int y, bool selected,
								  int prefix_width) {
	ListItemPos pos;

	pos.pill_width = UI_calcListPillWidth(font, text, truncated, layout->max_width, prefix_width);

	SDL_Rect pill_rect = {SCALE1(PADDING), y, pos.pill_width, layout->item_h};
	UI_drawListItemBg(screen, &pill_rect, selected);

	pos.text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	pos.text_y = y + (layout->item_h - TTF_FontHeight(font)) / 2;

	return pos;
}

void UI_renderListItemText(SDL_Surface* screen, ScrollTextState* scroll_state,
						   const char* text, TTF_Font* font,
						   int text_x, int text_y, int max_text_width,
						   bool selected) {
	SDL_Color text_color = UI_getListTextColor(selected);

	SDL_Rect old_clip;
	SDL_GetClipRect(screen, &old_clip);
	SDL_Rect clip = {text_x, text_y, max_text_width, TTF_FontHeight(font)};
	if (old_clip.w > 0 && old_clip.h > 0) {
		int left = clip.x > old_clip.x ? clip.x : old_clip.x;
		int top = clip.y > old_clip.y ? clip.y : old_clip.y;
		int right = (clip.x + clip.w) < (old_clip.x + old_clip.w) ? (clip.x + clip.w) : (old_clip.x + old_clip.w);
		int bottom = (clip.y + clip.h) < (old_clip.y + old_clip.h) ? (clip.y + clip.h) : (old_clip.y + old_clip.h);
		if (right > left && bottom > top) {
			clip = (SDL_Rect){left, top, right - left, bottom - top};
		} else {
			return;
		}
	}
	SDL_SetClipRect(screen, &clip);

	if (selected && scroll_state) {
		ScrollText_update(scroll_state, text, font, max_text_width,
						  text_color, screen, text_x, text_y, true);
	} else {
		SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font, text, text_color);
		if (text_surf) {
			SDL_Rect src = {0, 0, text_surf->w > max_text_width ? max_text_width : text_surf->w, text_surf->h};
			SDL_BlitSurface(text_surf, &src, screen, &(SDL_Rect){text_x, text_y, 0, 0});
			SDL_FreeSurface(text_surf);
		}
	}

	if (old_clip.w > 0 && old_clip.h > 0)
		SDL_SetClipRect(screen, &old_clip);
	else
		SDL_SetClipRect(screen, NULL);
}

// ============================================
// Badged Pill Rendering
// ============================================

ListItemBadgedPos UI_renderListItemPillBadged(
	SDL_Surface* screen, ListLayout* layout,
	TTF_Font* title_font, TTF_Font* subtitle_font, TTF_Font* badge_font,
	const char* text, const char* subtitle, char* truncated,
	int y, bool selected, int badge_width, int extra_subtitle_width) {
	ListItemBadgedPos pos;
	int item_h = SCALE1(PILL_SIZE) * 3 / 2;

	// Badge area: badge content + BUTTON_PADDING on each side
	int badge_area_w = badge_width > 0 ? badge_width + SCALE1(BUTTON_PADDING * 2) : 0;

	// Calculate title pill width (reduced max to leave room for badge area)
	int title_max_width = layout->max_width - badge_area_w;
	pos.pill_width = UI_calcListPillWidth(title_font, text, truncated, title_max_width, 0);

	// Expand pill if subtitle is wider than title
	if (subtitle && subtitle[0]) {
		int sub_w;
		TTF_SizeUTF8(subtitle_font, subtitle, &sub_w, NULL);
		sub_w += extra_subtitle_width;
		int sub_pill_w = MIN(title_max_width, sub_w + SCALE1(BUTTON_PADDING * 2));
		if (sub_pill_w > pos.pill_width)
			pos.pill_width = sub_pill_w;
	}

	if (selected) {
		int px = SCALE1(PADDING);

		if (badge_area_w > 0) {
			// Layer 1: THEME_COLOR2 outer capsule covering title + badge area
			int total_w = pos.pill_width + badge_area_w;
			int r = item_h / 3;
			if (r > total_w / 2)
				r = total_w / 2;
			if (item_h - 2 * r > 0) {
				SDL_FillRect(screen, &(SDL_Rect){px, y + r, total_w, item_h - 2 * r}, THEME_COLOR2);
			}
			for (int dy = 0; dy < r; dy++) {
				int yd = r - dy;
				int inset = r - (int)sqrtf((float)(r * r - yd * yd));
				int row_w = total_w - 2 * inset;
				if (row_w <= 0)
					continue;
				SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR2);
				SDL_FillRect(screen, &(SDL_Rect){px + inset, y + item_h - 1 - dy, row_w, 1}, THEME_COLOR2);
			}
		}

		// Layer 2 (or only layer): THEME_COLOR1 inner capsule for title area
		{
			int pw = pos.pill_width;
			int r = item_h / 3;
			if (r > pw / 2)
				r = pw / 2;
			if (item_h - 2 * r > 0) {
				SDL_FillRect(screen, &(SDL_Rect){px, y + r, pw, item_h - 2 * r}, THEME_COLOR1);
			}
			for (int dy = 0; dy < r; dy++) {
				int yd = r - dy;
				int inset = r - (int)sqrtf((float)(r * r - yd * yd));
				int row_w = pw - 2 * inset;
				if (row_w <= 0)
					continue;
				SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR1);
				SDL_FillRect(screen, &(SDL_Rect){px + inset, y + item_h - 1 - dy, row_w, 1}, THEME_COLOR1);
			}
		}
	}

	// Text positions: two rows vertically centered
	int text_start_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
	int title_h = TTF_FontHeight(title_font);
	int sub_h = TTF_FontHeight(subtitle_font);
	int total_text_h = title_h + sub_h;
	int top_gap = (item_h - total_text_h) / 2;

	pos.text_x = text_start_x;
	pos.text_y = y + top_gap;

	pos.subtitle_x = text_start_x;
	pos.subtitle_y = y + top_gap + title_h;

	// Badge position (centered vertically in capsule)
	pos.badge_x = SCALE1(PADDING) + pos.pill_width + SCALE1(BUTTON_PADDING);
	pos.badge_y = y + (item_h - TTF_FontHeight(badge_font)) / 2;

	// Account for right-side capsule radius reducing usable text width
	int r = item_h / 2;
	pos.text_max_width = pos.pill_width - SCALE1(BUTTON_PADDING) - r / 2;

	pos.total_width = pos.pill_width + badge_area_w;

	return pos;
}

// ============================================
// Scroll Helpers
// ============================================

void UI_adjustListScroll(int selected, int* scroll, int items_per_page) {
	if (selected < *scroll) {
		*scroll = selected;
	}
	if (selected >= *scroll + items_per_page) {
		*scroll = selected - items_per_page + 1;
	}
}

void UI_renderScrollIndicators(SDL_Surface* screen, int scroll, int items_per_page, int total_count) {
	if (total_count <= items_per_page)
		return;

	int hw = screen->w;
	int hh = screen->h;
	int ox = (hw - SCALE1(24)) / 2;

	if (scroll > 0) {
		GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE - BUTTON_MARGIN)});
	}
	if (scroll + items_per_page < total_count) {
		GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN)});
	}
}

// ============================================
// Pill Animation (non-threaded, for main-loop driven apps)
// ============================================

void UI_pillAnimInit(PillAnimState* state) {
	state->current_y = 0;
	state->target_y = 0;
	state->start_y = 0;
	state->frame = 0;
	state->total_frames = 3;
	state->active = false;
}

void UI_pillAnimSetTarget(PillAnimState* state, int target_y, bool animate) {
	if (target_y == state->current_y && !state->active)
		return;

	state->start_y = state->current_y;
	state->target_y = target_y;
	state->frame = 0;
	state->total_frames = animate ? 3 : 0;
	state->active = true;
}

int UI_pillAnimTick(PillAnimState* state) {
	if (!state->active)
		return state->current_y;

	if (state->frame >= state->total_frames) {
		state->current_y = state->target_y;
		state->active = false;
		return state->current_y;
	}

	state->frame++;
	float t = (state->total_frames > 0) ? ((float)state->frame / state->total_frames) : 1.0f;
	if (t > 1.0f)
		t = 1.0f;

	state->current_y = state->start_y + (int)((state->target_y - state->start_y) * t);
	return state->current_y;
}

bool UI_pillAnimIsActive(PillAnimState* state) {
	return state->active;
}
