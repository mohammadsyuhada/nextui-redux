#include <stdio.h>
#include <unistd.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "ui_components.h"
#include "utils.h"

// ============================================
// Rendering helpers
// ============================================

// ASSET_BUTTON/ASSET_HOLE sprite is 20x20 unscaled
#define BUTTON_SPRITE_SIZE 20
// Offset to center button sprite within a PILL_SIZE pill
#define BUTTON_INSET ((PILL_SIZE - BUTTON_SPRITE_SIZE) / 2)

static int getButtonWidth(char* label) {
	int w = 0;

	if (strlen(label) <= 2) {
		w = SCALE1(BUTTON_SPRITE_SIZE);
	} else {
		SDL_Surface* text = TTF_RenderUTF8_Blended(font.tiny, label, COLOR_BUTTON_TEXT);
		w = text->w + SCALE1(BUTTON_INSET) * 2;
		SDL_FreeSurface(text);
	}
	return w;
}

static void blitButton(char* label, SDL_Surface* dst, int pressed, int x, int y, int w) {
	SDL_Rect point = {x, y};
	SDL_Surface* text;

	int len = strlen(label);
	if (len <= 2) {
		text = TTF_RenderUTF8_Blended(len == 2 ? font.small : font.medium, label, COLOR_BUTTON_TEXT);
		GFX_blitAsset(pressed ? ASSET_BUTTON : ASSET_HOLE, NULL, dst, &point);
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){point.x + (SCALE1(BUTTON_SPRITE_SIZE) - text->w) / 2, point.y + (SCALE1(BUTTON_SPRITE_SIZE) - text->h) / 2});
	} else {
		text = TTF_RenderUTF8_Blended(font.tiny, label, COLOR_BUTTON_TEXT);
		w = w ? w : text->w + SCALE1(BUTTON_INSET) * 2;
		GFX_blitPill(pressed ? ASSET_BUTTON : ASSET_HOLE, dst, &(SDL_Rect){point.x, point.y, w, SCALE1(BUTTON_SPRITE_SIZE)});
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){point.x + (w - text->w) / 2, point.y + (SCALE1(BUTTON_SPRITE_SIZE) - text->h) / 2, text->w, text->h});
	}

	SDL_FreeSurface(text);
}

// ============================================
// Main
// ============================================

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	SDL_Surface* screen = GFX_init(MODE_MAIN);
	UI_showSplashScreen(screen, "Input");

	InitSettings();
	PAD_init();
	PWR_init();

	setup_signal_handlers();
	PWR_disableSleep();
	PWR_disablePowerOff();

	// one-time capability detection
	int has_L2 = (BUTTON_L2 != BUTTON_NA || CODE_L2 != CODE_NA || JOY_L2 != JOY_NA || AXIS_L2 != AXIS_NA);
	int has_R2 = (BUTTON_R2 != BUTTON_NA || CODE_R2 != CODE_NA || JOY_R2 != JOY_NA || AXIS_R2 != AXIS_NA);
	int has_L3 = (BUTTON_L3 != BUTTON_NA || CODE_L3 != CODE_NA || JOY_L3 != JOY_NA);
	int has_R3 = (BUTTON_R3 != BUTTON_NA || CODE_R3 != CODE_NA || JOY_R3 != JOY_NA);

	int has_volume = (BUTTON_PLUS != BUTTON_NA || CODE_PLUS != CODE_NA || JOY_PLUS != JOY_NA);
	int has_power = HAS_POWER_BUTTON;
	int has_menu = HAS_MENU_BUTTON;
	int has_both = (has_power && has_menu);

	int oy = SCALE1(PADDING);
	if (!has_L3 && !has_R3)
		oy += SCALE1(PILL_SIZE);

	bool quit = false;
	bool dirty = true;
	IndicatorType show_setting = INDICATOR_NONE;

	while (!quit && !app_quit) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_anyPressed() || PAD_anyJustReleased())
			dirty = true;
		if (PAD_isPressed(BTN_SELECT) && PAD_isPressed(BTN_START))
			quit = true;

		PWR_update(&dirty, &show_setting, NULL, NULL);

		if (UI_statusBarChanged())
			dirty = true;

		if (dirty) {
			GFX_clear(screen);

			UI_renderMenuBar(screen, "Input");

			// L group (centered over DPAD)
			{
				int y = oy + SCALE1(PILL_SIZE);
				int w = getButtonWidth("L1") + SCALE1(BUTTON_INSET) * 2;
				int ox = w;
				if (has_L2)
					w += getButtonWidth("L2") + SCALE1(BUTTON_INSET);

				int dpad_center = SCALE1(PADDING) + SCALE1(PILL_SIZE * 3) / 2;
				int x = dpad_center - w / 2;

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, w}, THEME_COLOR3, RGB_WHITE);

				blitButton("L1", screen, PAD_isPressed(BTN_L1), x + SCALE1(BUTTON_INSET), y + SCALE1(BUTTON_INSET), 0);
				if (has_L2)
					blitButton("L2", screen, PAD_isPressed(BTN_L2), x + ox, y + SCALE1(BUTTON_INSET), 0);
			}

			// R group (centered over ABXY)
			{
				int y = oy + SCALE1(PILL_SIZE);
				int w = getButtonWidth("R1") + SCALE1(BUTTON_INSET) * 2;
				int ox = w;
				if (has_R2)
					w += getButtonWidth("R2") + SCALE1(BUTTON_INSET);

				int abxy_center = FIXED_WIDTH - SCALE1(PADDING) - SCALE1(PILL_SIZE * 3) / 2;
				int x = abxy_center - w / 2;

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, w}, THEME_COLOR3, RGB_WHITE);

				blitButton(has_R2 ? "R2" : "R1", screen, PAD_isPressed(has_R2 ? BTN_R2 : BTN_R1), x + SCALE1(BUTTON_INSET), y + SCALE1(BUTTON_INSET), 0);
				if (has_R2)
					blitButton("R1", screen, PAD_isPressed(BTN_R1), x + ox, y + SCALE1(BUTTON_INSET), 0);
			}

			// DPAD group
			{
				int x = SCALE1(PADDING + PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE * 2 + PILL_SIZE / 2);
				int o = SCALE1(BUTTON_INSET);

				SDL_FillRect(screen, &(SDL_Rect){x, y + SCALE1(PILL_SIZE / 2), SCALE1(PILL_SIZE), SCALE1(PILL_SIZE * 2)}, THEME_COLOR3);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("U", screen, PAD_isPressed(BTN_DPAD_UP), x + o, y + o, 0);

				y += SCALE1(PILL_SIZE * 2);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("D", screen, PAD_isPressed(BTN_DPAD_DOWN), x + o, y + o, 0);

				x -= SCALE1(PILL_SIZE);
				y -= SCALE1(PILL_SIZE);

				SDL_FillRect(screen, &(SDL_Rect){x + SCALE1(PILL_SIZE / 2), y, SCALE1(PILL_SIZE * 2), SCALE1(PILL_SIZE)}, THEME_COLOR3);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("L", screen, PAD_isPressed(BTN_DPAD_LEFT), x + o, y + o, 0);

				x += SCALE1(PILL_SIZE * 2);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("R", screen, PAD_isPressed(BTN_DPAD_RIGHT), x + o, y + o, 0);
			}

			// ABXY group
			{
				int x = FIXED_WIDTH - SCALE1(PADDING + PILL_SIZE * 3) + SCALE1(PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE * 2 + PILL_SIZE / 2);
				int o = SCALE1(BUTTON_INSET);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("X", screen, PAD_isPressed(BTN_X), x + o, y + o, 0);

				y += SCALE1(PILL_SIZE * 2);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("B", screen, PAD_isPressed(BTN_B), x + o, y + o, 0);

				x -= SCALE1(PILL_SIZE);
				y -= SCALE1(PILL_SIZE);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("Y", screen, PAD_isPressed(BTN_Y), x + o, y + o, 0);

				x += SCALE1(PILL_SIZE * 2);
				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("A", screen, PAD_isPressed(BTN_A), x + o, y + o, 0);
			}

			// VOLUME group
			if (has_volume) {
				int x = (FIXED_WIDTH - SCALE1(99)) / 2;
				int y = oy + SCALE1(PILL_SIZE);
				int w = SCALE1(42);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, SCALE1(98)}, THEME_COLOR3, RGB_WHITE);
				x += SCALE1(BUTTON_INSET);
				y += SCALE1(BUTTON_INSET);
				blitButton("VOL. -", screen, PAD_isPressed(BTN_MINUS), x, y, w);
				x += w + SCALE1(BUTTON_INSET);
				blitButton("VOL. +", screen, PAD_isPressed(BTN_PLUS), x, y, w);
			}

			// SYSTEM group
			if (has_power || has_menu) {
				int bw = 42;
				int pw = has_both ? (bw * 2 + BUTTON_INSET * 3) : (bw + BUTTON_INSET * 2);

				int x = (FIXED_WIDTH - SCALE1(pw)) / 2;
				int y = oy + SCALE1(PILL_SIZE * 3);
				int w = SCALE1(bw);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, SCALE1(pw)}, THEME_COLOR3, RGB_WHITE);
				x += SCALE1(BUTTON_INSET);
				y += SCALE1(BUTTON_INSET);
				if (has_menu) {
					blitButton("MENU", screen, PAD_isPressed(BTN_MENU), x, y, w);
					x += w + SCALE1(BUTTON_INSET);
				}
				if (has_power) {
					blitButton("POWER", screen, PAD_isPressed(BTN_POWER), x, y, w);
				}
			}

			// META group
			{
				int bw = SCALE1(42);
				int pw = SCALE1(BUTTON_INSET) * 3 + bw * 2;

				int x = (FIXED_WIDTH - pw) / 2;
				int y = oy + SCALE1(PILL_SIZE * 5);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, pw}, THEME_COLOR3, RGB_WHITE);
				x += SCALE1(BUTTON_INSET);
				y += SCALE1(BUTTON_INSET);
				blitButton("SELECT", screen, PAD_isPressed(BTN_SELECT), x, y, bw);
				x += bw + SCALE1(BUTTON_INSET);
				blitButton("START", screen, PAD_isPressed(BTN_START), x, y, bw);
			}

			// L3
			if (has_L3) {
				int x = SCALE1(PADDING + PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE * 6);
				int o = SCALE1(BUTTON_INSET);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("L3", screen, PAD_isPressed(BTN_L3), x + o, y + o, 0);
			}

			// R3
			if (has_R3) {
				int x = FIXED_WIDTH - SCALE1(PADDING + PILL_SIZE * 3) + SCALE1(PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE * 6);
				int o = SCALE1(BUTTON_INSET);

				GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, 0}, THEME_COLOR3, RGB_WHITE);
				blitButton("R3", screen, PAD_isPressed(BTN_R3), x + o, y + o, 0);
			}

			UI_renderButtonHintBar(screen, (char*[]){"SELECT+START", "QUIT", NULL});

			GFX_flip(screen);
			dirty = false;
		} else {
			GFX_sync();
		}
	}

	PWR_enableSleep();
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
