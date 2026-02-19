#include "ui_components.h"
#include "api.h"
#include "defines.h"

void UI_renderConfirmDialog(SDL_Surface* dst, const char* title,
							const char* subtitle) {
	int padding_x = SCALE1(PADDING * 4);
	int content_w = dst->w - padding_x * 2;

	SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format, 0, 0, 0));

	int title_h = TTF_FontHeight(font.large);
	int total_h = title_h;
	if (subtitle)
		total_h += SCALE1(BUTTON_MARGIN) + TTF_FontHeight(font.small);

	int y = (dst->h - total_h) / 2;

	// Title
	SDL_Rect title_rect = {padding_x, y, content_w, title_h};
	GFX_blitMessage(font.large, (char*)title, dst, &title_rect);

	// Subtitle (optional)
	if (subtitle) {
		int sub_h = TTF_FontHeight(font.small);
		y += title_h + SCALE1(BUTTON_MARGIN);
		SDL_Rect sub_rect = {padding_x, y, content_w, sub_h};
		GFX_blitMessage(font.small, (char*)subtitle, dst, &sub_rect);
	}

	GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "CONFIRM", NULL}, 0,
						dst, 1);
}

void UI_calcImageFit(int img_w, int img_h, int max_w, int max_h,
					 int* out_w, int* out_h) {
	double aspect_ratio = (double)img_h / img_w;
	int new_w = max_w;
	int new_h = (int)(new_w * aspect_ratio);

	if (new_h > max_h) {
		new_h = max_h;
		new_w = (int)(new_h / aspect_ratio);
	}

	*out_w = new_w;
	*out_h = new_h;
}

SDL_Surface* UI_convertSurface(SDL_Surface* surface, SDL_Surface* screen) {
	SDL_Surface* converted =
		SDL_ConvertSurfaceFormat(surface, screen->format->format, 0);
	if (converted) {
		SDL_FreeSurface(surface);
		return converted;
	}
	return surface;
}

void UI_renderCenteredMessage(SDL_Surface* dst, const char* message) {
	SDL_Rect fullscreen_rect = {0, 0, dst->w, dst->h};
	GFX_blitMessage(font.large, (char*)message, dst, &fullscreen_rect);
}

int UI_renderMenuBar(SDL_Surface* screen, const char* title, int show_setting) {
	// Semi-transparent bar background (cached between calls)
	static SDL_Surface* menu_bar = NULL;
	int bar_h = SCALE1(PADDING + PILL_SIZE);
	if (!menu_bar || menu_bar->w != screen->w || menu_bar->h != bar_h) {
		if (menu_bar)
			SDL_FreeSurface(menu_bar);
		menu_bar = SDL_CreateRGBSurface(SDL_SWSURFACE, screen->w, bar_h, 32,
										0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
		SDL_FillRect(menu_bar, NULL, SDL_MapRGBA(menu_bar->format, 0, 0, 0, 178));
		SDL_SetSurfaceBlendMode(menu_bar, SDL_BLENDMODE_BLEND);
	}
	SDL_BlitSurface(menu_bar, NULL, screen, &(SDL_Rect){0, 0});

	// Hardware group (right side)
	int ow = GFX_blitHardwareGroup(screen, show_setting);

	// Title text (left side, no pill)
	if (title && title[0]) {
		int max_title_w = screen->w - ow - SCALE1(PADDING * 2);
		char truncated[256];
		GFX_truncateText(font.medium, title, truncated, max_title_w, 0);

		SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_GRAY);
		if (text) {
			int text_y = SCALE1(PADDING) + (SCALE1(PILL_SIZE) - text->h) / 2;
			SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), text_y});
			SDL_FreeSurface(text);
		}
	}

	return ow;
}
